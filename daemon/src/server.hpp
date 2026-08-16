#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Newline-delimited JSON over a unix socket. Every connected client receives
// state pushes; requests are single JSON objects with a "cmd" field.
class ControlServer {
public:
  using Handler = std::function<std::string(const std::string& requestJson)>;  // returns reply JSON (may be empty)
  ~ControlServer() { stop(); }
  bool start(const std::string& socketPath, Handler handler, std::string* err);
  void stop();
  void broadcast(const std::string& line);
  int clients();

private:
  void run();
  void serveClient(int fd);
  std::string path_;
  int listenFd_ = -1;
  Handler handler_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mu_;
  std::vector<int> clients_;
};
