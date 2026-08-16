#include "server.hpp"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>

bool ControlServer::start(const std::string& socketPath, Handler handler, std::string* err) {
  stop();
  path_ = socketPath;
  handler_ = std::move(handler);
  listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listenFd_ < 0) { if (err) *err = strerror(errno); return false; }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
  unlink(path_.c_str());
  if (bind(listenFd_, (sockaddr*)&addr, sizeof addr) < 0) { if (err) *err = std::string("bind: ") + strerror(errno); close(listenFd_); listenFd_ = -1; return false; }
  if (listen(listenFd_, 8) < 0) { if (err) *err = std::string("listen: ") + strerror(errno); close(listenFd_); listenFd_ = -1; return false; }
  stop_ = false;
  thread_ = std::thread([this] { run(); });
  return true;
}

void ControlServer::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (listenFd_ >= 0) {
    // Only remove the socket file if it is still ours (a newer instance may
    // have replaced it while we were shutting down).
    struct stat a{}, b{};
    bool ours = fstat(listenFd_, &a) == 0 && stat(path_.c_str(), &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev;
    close(listenFd_);
    listenFd_ = -1;
    if (ours) unlink(path_.c_str());
  }
  std::lock_guard<std::mutex> lk(mu_);
  for (int fd : clients_) close(fd);
  clients_.clear();
}

int ControlServer::clients() {
  std::lock_guard<std::mutex> lk(mu_);
  return (int)clients_.size();
}

void ControlServer::broadcast(const std::string& line) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string msg = line + "\n";
  for (auto it = clients_.begin(); it != clients_.end();) {
    ssize_t n = send(*it, msg.data(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN) { close(*it); it = clients_.erase(it); }
    else ++it;
  }
}

void ControlServer::run() {
  std::map<int, std::string> buffers;
  while (!stop_) {
    std::vector<pollfd> pfds;
    pfds.push_back({ listenFd_, POLLIN, 0 });
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (int fd : clients_) pfds.push_back({ fd, POLLIN, 0 });
    }
    int r = poll(pfds.data(), pfds.size(), 200);
    if (r <= 0) continue;
    if (pfds[0].revents & POLLIN) {
      int c = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (c >= 0) {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.push_back(c);
        // Greet with the current state so clients don't need to ask.
        std::string reply = handler_("{\"cmd\":\"get\"}");
        if (!reply.empty()) { reply += "\n"; send(c, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }
      }
    }
    for (size_t i = 1; i < pfds.size(); i++) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int fd = pfds[i].fd;
      char buf[4096];
      ssize_t n = recv(fd, buf, sizeof buf, 0);
      if (n <= 0) {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
        buffers.erase(fd);
        close(fd);
        continue;
      }
      std::string& acc = buffers[fd];
      acc.append(buf, n);
      size_t pos;
      while ((pos = acc.find('\n')) != std::string::npos) {
        std::string line = acc.substr(0, pos);
        acc.erase(0, pos + 1);
        if (line.empty()) continue;
        std::string reply = handler_(line);
        if (!reply.empty()) { reply += "\n"; send(fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }
      }
    }
  }
}
