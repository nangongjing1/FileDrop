#include "dns_server.h"

#include <cstring>
#include <thread>

#include "dns_protocol.h"
#include "applog.h"

DnsServer::~DnsServer() { stop(); }

bool DnsServer::parseIp(const std::string& text, uint32_t& out) {
  in_addr addr{};
  if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
  out = addr.s_addr;
  return true;
}

bool DnsServer::start(const std::string& domain, const std::vector<std::string>& aliases, uint32_t fallbackIp,
                      const std::string& upstream, uint16_t port, std::string& err) {
  names_.clear();
  std::string primary = dnsproto::normalizeDomain(domain);
  if (!primary.empty()) names_.push_back(primary);
  for (const auto& alias : aliases) {
    std::string normalized = dnsproto::normalizeDomain(alias);
    if (normalized.empty()) continue;
    bool duplicate = false;
    for (const auto& name : names_) {
      if (name == normalized) duplicate = true;
    }
    if (!duplicate) names_.push_back(normalized);
  }
  fallbackIp_ = fallbackIp;
  upstream_ = upstream;

  socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ == kInvalidSocket) {
    err = "无法创建 DNS 套接字";
    return false;
  }

  int reuse = 1;
#ifdef _WIN32
  // Windows 下 SO_REUSEADDR 会允许与其它进程共享端口，导致收不到数据报；
  // 用独占模式才能在端口被占用时明确报错。
  setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#else
  setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#endif
  net::setRecvTimeout(socket_, 1000);

  auto tryBind = [this, port](uint32_t address) -> bool {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = address;
    addr.sin_port = htons(port);
    return bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  };

  if (!tryBind(htonl(INADDR_ANY))) {
    // 端口被其它服务(如 Internet 连接共享)占用时，尝试只绑定到本机对外地址。
    if (!tryBind(fallbackIp)) {
      net::closeSocket(socket_);
      socket_ = kInvalidSocket;
      err = "无法绑定 UDP 端口 " + std::to_string(port) + "，可能已被其它程序占用";
      return false;
    }
  }

  running_ = true;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void DnsServer::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (socket_ != kInvalidSocket) {
    net::closeSocket(socket_);
    socket_ = kInvalidSocket;
  }
}

void DnsServer::loop() {
  std::vector<uint8_t> buffer(dnsproto::kMaxQuery);
  while (running_) {
    sockaddr_in client{};
#ifdef _WIN32
    int clientLen = sizeof client;
#else
    socklen_type clientLen = sizeof client;
#endif
    int received = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                            reinterpret_cast<sockaddr*>(&client), &clientLen);
    if (received <= 0) continue;
    if (received < 12) continue;

    std::vector<uint8_t> query(buffer.begin(), buffer.begin() + received);
    std::string qname;
    uint16_t qtype = 0;
    size_t questionEnd = 0;
    if (!dnsproto::parseQuestion(query, qname, qtype, questionEnd)) continue;
    (void)questionEnd;

    std::string normalized = dnsproto::normalizeDomain(qname);
    bool matched = false;
    for (const auto& name : names_) {
      if (name == normalized) {
        matched = true;
        break;
      }
    }

    if (matched) {
      answerLocal(query, client, clientLen, qtype);
    } else {
      std::thread([this, query = std::move(query), client, clientLen]() mutable {
        forwardQuery(std::move(query), client, clientLen);
      }).detach();
    }
  }
}

uint32_t DnsServer::resolveAddress(const std::string& clientIp) const {
  if (answerProvider_) {
    std::string ip = answerProvider_(clientIp);
    uint32_t value = 0;
    if (parseIp(ip, value)) return value;
  }
  return fallbackIp_;
}

void DnsServer::answerLocal(const std::vector<uint8_t>& query, const sockaddr_in& client, socklen_type clientLen,
                            uint16_t qtype) {
  char peer[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, const_cast<in_addr*>(&client.sin_addr), peer, sizeof peer);

  std::string qname;
  uint16_t parsedType = 0;
  size_t questionEnd = 0;
  if (dnsproto::parseQuestion(query, qname, parsedType, questionEnd)) {
    applog::info("DNS " + qname + " type=" + std::to_string(parsedType) + " 来自 " + peer);
  }

  uint32_t answerIp = resolveAddress(peer);

  auto response = dnsproto::buildAnswer(query, qtype, answerIp);
  if (response.empty()) return;
  sendto(socket_, reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0,
         reinterpret_cast<const sockaddr*>(&client), clientLen);
}

void DnsServer::forwardQuery(std::vector<uint8_t> query, sockaddr_in client, socklen_type clientLen) {
  auto makeFailure = [&](uint8_t rcode) {
    auto response = dnsproto::buildFailure(query, rcode);
    if (response.empty()) return;
    sendto(socket_, reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0,
           reinterpret_cast<const sockaddr*>(&client), clientLen);
  };

  uint32_t upstreamIp = 0;
  if (upstream_.empty() || !parseIp(upstream_, upstreamIp)) {
    makeFailure(2);  // SERVFAIL
    return;
  }

  socket_t upstreamSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (upstreamSocket == kInvalidSocket) {
    makeFailure(2);
    return;
  }
  net::setRecvTimeout(upstreamSocket, 1500);

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(53);
  target.sin_addr.s_addr = upstreamIp;

  if (sendto(upstreamSocket, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
             reinterpret_cast<const sockaddr*>(&target), sizeof target) <= 0) {
    net::closeSocket(upstreamSocket);
    makeFailure(2);
    return;
  }

  std::vector<uint8_t> response(dnsproto::kMaxQuery);
  int received = recv(upstreamSocket, reinterpret_cast<char*>(response.data()), static_cast<int>(response.size()), 0);
  net::closeSocket(upstreamSocket);
  if (received <= 0) {
    makeFailure(2);
    return;
  }
  if (!running_) return;
  sendto(socket_, reinterpret_cast<const char*>(response.data()), received, 0,
         reinterpret_cast<const sockaddr*>(&client), clientLen);
}
