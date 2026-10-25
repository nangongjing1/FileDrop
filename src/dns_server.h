#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "http_server.h"

class DnsServer {
 public:
  ~DnsServer();

  bool start(const std::string& domain, uint32_t answerIp, const std::string& upstream, uint16_t port,
             std::string& err);
  void stop();
  bool running() const { return running_; }

  static bool parseIp(const std::string& text, uint32_t& out);

 private:
  void loop();
  void answerLocal(const std::vector<uint8_t>& query, const sockaddr_in& client, socklen_type clientLen,
                   uint16_t qtype);
  void forwardQuery(std::vector<uint8_t> query, sockaddr_in client, socklen_type clientLen);

  socket_t socket_ = kInvalidSocket;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::string domain_;
  uint32_t answerIp_ = 0;
  std::string upstream_;
};
