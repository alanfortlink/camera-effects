#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Newline-delimited JSON over a unix socket. Every connected client receives
// state pushes; requests are single JSON objects with a "cmd" field. Requests
// carry the client's id (its fd: unique while it is connected) so per-client
// state can be kept, and OnClose reports the id when the client goes away.
// Both callbacks run on the server thread.
class ControlServer {
public:
  using Handler = std::function<std::string(int client, const std::string& requestJson)>;  // returns reply JSON (may be empty)
  using OnClose = std::function<void(int client)>;
  ~ControlServer() { stop(); }
  bool start(const std::string& socketPath, Handler handler, OnClose onClose, std::string* err);
  void stop();
  void broadcast(const std::string& line);
  int clients();

private:
  void run();
  std::string path_;
  int listenFd_ = -1;
  Handler handler_;
  OnClose onClose_;
  void dropClient(int fd, std::map<int, std::string>& buffers);
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mu_;
  std::vector<int> clients_;
};
