#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "http_server.h"

class DnsServer {
 public:
  ~DnsServer();

  // 返回给客户端设备 IP 的函数；默认返回 fallbackIp。
  using AnswerProvider = std::function<std::string(const std::string& clientIp)>;

  bool start(const std::string& domain, const std::vector<std::string>& aliases, uint32_t fallbackIp,
             const std::string& upstream, uint16_t port, std::string& err);
  void setAnswerProvider(AnswerProvider provider) { answerProvider_ = std::move(provider); }
  void stop();
  bool running() const { return running_; }
  std::vector<std::string> names() const { return names_; }

  static bool parseIp(const std::string& text, uint32_t& out);

 private:
  void loop();
  void answerLocal(const std::vector<uint8_t>& query, const sockaddr_in& client, socklen_type clientLen,
                   uint16_t qtype);
  void forwardQuery(std::vector<uint8_t> query, sockaddr_in client, socklen_type clientLen);
  uint32_t resolveAddress(const std::string& clientIp) const;

  socket_t socket_ = kInvalidSocket;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::vector<std::string> names_;
  uint32_t fallbackIp_ = 0;
  std::string upstream_;
  AnswerProvider answerProvider_;
};
