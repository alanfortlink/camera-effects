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

bool ControlServer::start(const std::string& socketPath, Handler handler, OnClose onClose, std::string* err) {
  stop();
  path_ = socketPath;
  handler_ = std::move(handler);
  onClose_ = std::move(onClose);
  sockaddr_un addr{};
  if (path_.size() >= sizeof(addr.sun_path)) { if (err) *err = "socket path too long: " + path_; return false; }
  listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listenFd_ < 0) { if (err) *err = strerror(errno); return false; }
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
    // The socket file is ours to remove: the daemon holds the instance lock
    // until it exits, so no newer instance can have bound this path yet.
    close(listenFd_);
    listenFd_ = -1;
    unlink(path_.c_str());
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
  // A dead client is only shut down here: run() sees the hangup and drops it,
  // so every disconnect (and its OnClose) happens on the server thread.
  for (int fd : clients_) {
    ssize_t n = send(fd, msg.data(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && errno != EAGAIN) shutdown(fd, SHUT_RDWR);
  }
}

void ControlServer::sendTo(int client, const std::string& line) {
  std::lock_guard<std::mutex> lk(mu_);
  if (std::find(clients_.begin(), clients_.end(), client) == clients_.end()) return;  // gone (its fd number may be someone else's by now)
  std::string msg = line + "\n";
  ssize_t n = send(client, msg.data(), msg.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  if (n < 0 && errno != EAGAIN) shutdown(client, SHUT_RDWR);
}

void ControlServer::dropClient(int fd, std::map<int, std::string>& buffers) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
  }
  buffers.erase(fd);
  if (onClose_) onClose_(fd);  // before close(): the fd number is not reused until then
  close(fd);
}

void ControlServer::run() {
  const size_t kMaxRequest = 64 * 1024;
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
        std::string reply = handler_(c, "{\"cmd\":\"get\"}");
        if (!reply.empty()) { reply += "\n"; send(c, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }
      }
    }
    for (size_t i = 1; i < pfds.size(); i++) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int fd = pfds[i].fd;
      char buf[4096];
      ssize_t n = recv(fd, buf, sizeof buf, 0);
      if (n <= 0) { dropClient(fd, buffers); continue; }
      std::string& acc = buffers[fd];
      acc.append(buf, n);
      if (acc.size() > kMaxRequest) { dropClient(fd, buffers); continue; }  // no newline in 64 KiB: not a client we understand
      size_t pos;
      while ((pos = acc.find('\n')) != std::string::npos) {
        std::string line = acc.substr(0, pos);
        acc.erase(0, pos + 1);
        if (line.empty()) continue;
        std::string reply = handler_(fd, line);
        if (!reply.empty()) { reply += "\n"; send(fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }
      }
    }
  }
}
