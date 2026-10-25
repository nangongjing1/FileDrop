#include "http_server.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "applog.h"
#include "util.h"

#ifndef _WIN32
#include <cerrno>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace net {

bool initSockets(std::string& err) {
#ifdef _WIN32
  WSADATA data;
  int result = WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    err = "WSAStartup failed: " + std::to_string(result);
    return false;
  }
#else
  (void)err;
#endif
  return true;
}

void cleanupSockets() {
#ifdef _WIN32
  WSACleanup();
#endif
}

bool setRecvTimeout(socket_t s, int ms) {
#ifdef _WIN32
  DWORD timeout = static_cast<DWORD>(ms);
  return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout) == 0;
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
#endif
}

bool setSendTimeout(socket_t s, int ms) {
#ifdef _WIN32
  DWORD timeout = static_cast<DWORD>(ms);
  return setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout) == 0;
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
#endif
}

void closeSocket(socket_t s) {
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

bool sendAll(socket_t s, const void* data, size_t len) {
  const char* ptr = static_cast<const char*>(data);
  size_t remaining = len;
  while (remaining > 0) {
    int chunk = static_cast<int>(std::min<size_t>(remaining, 1 << 20));
#ifdef _WIN32
    int sent = send(s, ptr, chunk, 0);
#else
    ssize_t sent = send(s, ptr, chunk, MSG_NOSIGNAL);
#endif
    if (sent <= 0) return false;
    ptr += sent;
    remaining -= static_cast<size_t>(sent);
  }
  return true;
}

bool wouldBlock() {
#ifdef _WIN32
  int code = WSAGetLastError();
  return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

}  // namespace net

namespace {

const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
  }
}

std::string peerIp(const sockaddr_in& addr) {
  char text[INET_ADDRSTRLEN] = {0};
  if (inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text)) return text;
  return "0.0.0.0";
}

}  // namespace

std::string HttpRequest::header(const std::string& lowerName) const {
  auto it = headers.find(lowerName);
  return it == headers.end() ? std::string() : it->second;
}

std::string HttpRequest::param(const std::string& key, const std::string& def) const {
  auto it = params.find(key);
  return it == params.end() ? def : it->second;
}

HeadParseResult httpParseRequestHead(const std::string& head, HttpRequest& request) {
  size_t lineEnd = head.find("\r\n");
  std::string requestLine = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
  std::istringstream requestStream(requestLine);
  std::string method, target, version;
  requestStream >> method >> target >> version;
  if (method.empty() || target.empty()) return HeadParseResult::BadRequest;
  if (!version.empty() && version.rfind("HTTP/", 0) != 0) return HeadParseResult::BadRequest;

  request.method = method;
  request.target = target;

  size_t queryPos = target.find('?');
  std::string rawPath = queryPos == std::string::npos ? target : target.substr(0, queryPos);
  request.query = queryPos == std::string::npos ? std::string() : target.substr(queryPos + 1);
  request.path = util::urlDecode(rawPath);

  size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
  while (pos < head.size()) {
    size_t end = head.find("\r\n", pos);
    if (end == std::string::npos) end = head.size();
    std::string line = head.substr(pos, end - pos);
    pos = end + 2;
    if (line.empty()) continue;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = util::toLower(util::trim(line.substr(0, colon)));
    std::string value = util::trim(line.substr(colon + 1));
    if (key.empty()) continue;
    auto it = request.headers.find(key);
    if (it == request.headers.end()) {
      request.headers.emplace(std::move(key), std::move(value));
    } else {
      it->second += ", " + value;
    }
  }

  if (!request.query.empty()) {
    size_t start = 0;
    while (start <= request.query.size()) {
      size_t amp = request.query.find('&', start);
      std::string pair = request.query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
      if (!pair.empty()) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
          request.params[util::urlDecode(pair)] = "";
        } else {
          request.params[util::urlDecode(pair.substr(0, eq))] = util::urlDecode(pair.substr(eq + 1));
        }
      }
      if (amp == std::string::npos) break;
      start = amp + 1;
    }
  }

  std::string transferEncoding = util::toLower(request.header("transfer-encoding"));
  if (!transferEncoding.empty() && transferEncoding.find("chunked") != std::string::npos) {
    return HeadParseResult::ChunkedNotSupported;
  }

  std::string lengthHeader = request.header("content-length");
  if (!lengthHeader.empty()) {
    try {
      request.contentLength = std::stoull(lengthHeader);
    } catch (...) {
      return HeadParseResult::BadRequest;
    }
  }
  return HeadParseResult::Ok;
}

HttpResponse::HttpResponse(socket_t socket) : socket_(socket) {}

void HttpResponse::addHeader(const std::string& name, const std::string& value) {
  headers_.emplace_back(name, value);
}

bool HttpResponse::begin(int status, const std::string& contentType, uint64_t contentLength) {
  if (started_) return false;
  started_ = true;
  expected_ = contentLength;

  std::string head;
  head.reserve(256);
  head += "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n";
  head += "Server: Filedrop/1.0\r\n";
  head += "Connection: close\r\n";
  head += "X-Content-Type-Options: nosniff\r\n";
  if (!contentType.empty()) head += "Content-Type: " + contentType + "\r\n";
  if (contentLength != net::kNoContentLength) {
    head += "Content-Length: " + std::to_string(contentLength) + "\r\n";
  }
  for (const auto& kv : headers_) {
    head += kv.first + ": " + kv.second + "\r\n";
  }
  head += "\r\n";

  if (!net::sendAll(socket_, head.data(), head.size())) {
    broken_ = true;
    return false;
  }
  return true;
}

bool HttpResponse::write(const void* data, size_t len) {
  if (!started_ || broken_ || len == 0) return !broken_;
  if (expected_ != net::kNoContentLength && written_ + len > expected_) {
    len = static_cast<size_t>(expected_ - written_);
  }
  if (!net::sendAll(socket_, data, len)) {
    broken_ = true;
    return false;
  }
  written_ += len;
  return true;
}

bool HttpResponse::write(const std::string& data) { return write(data.data(), data.size()); }

bool HttpResponse::sendText(int status, const std::string& body, const std::string& contentType) {
  if (!begin(status, contentType, body.size())) return false;
  return write(body);
}

bool HttpResponse::sendEmpty(int status) {
  return begin(status, "text/plain; charset=utf-8", 0);
}

void HttpServer::get(const std::string& path, Handler handler) {
  routes_.push_back({"GET", path, false, std::move(handler)});
}

void HttpServer::post(const std::string& path, Handler handler) {
  routes_.push_back({"POST", path, false, std::move(handler)});
}

void HttpServer::put(const std::string& path, Handler handler) {
  routes_.push_back({"PUT", path, false, std::move(handler)});
}

void HttpServer::onPrefix(const std::string& method, const std::string& prefix, Handler handler) {
  routes_.push_back({method, prefix, true, std::move(handler)});
}

bool HttpServer::start(const std::string& bindAddress, int port, std::string& err) {
  if (listenSocket_ != kInvalidSocket) {
    err = "server already started";
    return false;
  }

  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocket) {
    err = "socket() failed";
    return false;
  }

  int reuse = 1;
#ifdef _WIN32
  // Windows 上 SO_REUSEADDR 会允许与其它进程共用端口，导致监听成功却收不到连接，
  // 因此改用独占模式，端口被占用时能明确失败并回退到备用端口。
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bindAddress.empty() || bindAddress == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
    net::closeSocket(s);
    err = "invalid bind address: " + bindAddress;
    return false;
  }

  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    net::closeSocket(s);
    err = "bind failed on port " + std::to_string(port);
    return false;
  }

  if (listen(s, 128) != 0) {
    net::closeSocket(s);
    err = "listen failed";
    return false;
  }

  sockaddr_in bound{};
#ifdef _WIN32
  int boundLen = sizeof bound;
#else
  socklen_t boundLen = sizeof bound;
#endif
  if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
    port_ = ntohs(bound.sin_port);
  } else {
    port_ = port;
  }

  listenSocket_ = s;
  running_ = true;
  acceptThread_ = std::thread([this] { acceptLoop(); });
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) return;
  if (listenSocket_ != kInvalidSocket) {
    net::closeSocket(listenSocket_);
    listenSocket_ = kInvalidSocket;
  }
  if (acceptThread_.joinable()) acceptThread_.join();
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::acceptLoop() {
  while (running_) {
    sockaddr_in addr{};
#ifdef _WIN32
    int addrLen = sizeof addr;
#else
    socklen_t addrLen = sizeof addr;
#endif
    socket_t client = accept(listenSocket_, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (client == kInvalidSocket) {
      if (!running_) break;
      continue;
    }
    std::string ip = peerIp(addr);
    activeConnections_++;
    std::thread([this, client, ip]() {
      handleConnection(client, ip);
      activeConnections_--;
    }).detach();
  }
}

const HttpServer::Route* HttpServer::matchRoute(const std::string& method, const std::string& path) const {
  for (const auto& route : routes_) {
    if (route.method != method) continue;
    if (!route.prefix) {
      if (route.path == path) return &route;
    } else if (path.rfind(route.path, 0) == 0) {
      return &route;
    }
  }
  return nullptr;
}

void HttpServer::handleConnection(socket_t client, std::string remoteIp) {
  net::setRecvTimeout(client, 30000);
  net::setSendTimeout(client, 30000);

  std::string buffer;
  buffer.reserve(8192);
  size_t headEnd = std::string::npos;
  while (true) {
    headEnd = buffer.find("\r\n\r\n");
    if (headEnd != std::string::npos) break;
    if (buffer.size() > 65536) {
      net::closeSocket(client);
      return;
    }
    char temp[4096];
    int received = recv(client, temp, sizeof temp, 0);
    if (received <= 0) {
      net::closeSocket(client);
      return;
    }
    buffer.append(temp, static_cast<size_t>(received));
  }

  std::string head = buffer.substr(0, headEnd);
  buffer.erase(0, headEnd + 4);

  HttpRequest request;
  request.remoteIp = std::move(remoteIp);

  HeadParseResult parsed = httpParseRequestHead(head, request);
  if (parsed == HeadParseResult::BadRequest) {
    HttpResponse response(client);
    response.sendText(400, "{\"ok\":false,\"error\":\"请求格式无效\"}");
    net::closeSocket(client);
    return;
  }
  if (parsed == HeadParseResult::ChunkedNotSupported) {
    HttpResponse response(client);
    response.sendText(411, "{\"ok\":false,\"error\":\"需要 Content-Length\"}");
    net::closeSocket(client);
    return;
  }

  bool expectContinue = util::toLower(request.header("expect")).find("100-continue") != std::string::npos;
  // 跳过管理端自身的轮询/SSE 请求，避免把日志刷满
  const std::string& logTarget = request.target;
  bool noisyAdminPoll = logTarget.rfind("/api/admin/state", 0) == 0 ||
                        logTarget.rfind("/api/admin/events", 0) == 0 ||
                        logTarget.rfind("/api/admin/log", 0) == 0;
  if (!noisyAdminPoll) {
    applog::info("HTTP " + request.method + " " + request.target + " 来自 " + request.remoteIp +
                 " Host=" + request.header("host") + " UA=\"" + request.header("user-agent") + "\"");
  }
  if (expectContinue) {
    net::sendAll(client, "HTTP/1.1 100 Continue\r\n\r\n", 25);
  }

  request.readBody = [client, leftover = std::move(buffer)](char* out, size_t capacity) mutable -> size_t {
    if (!leftover.empty()) {
      size_t take = std::min(capacity, leftover.size());
      std::memcpy(out, leftover.data(), take);
      leftover.erase(0, take);
      return take;
    }
    int received = recv(client, out, static_cast<int>(std::min<size_t>(capacity, 1 << 20)), 0);
    if (received <= 0) return 0;
    return static_cast<size_t>(received);
  };

  HttpResponse response(client);
  const Route* route = matchRoute(request.method, request.path);
  if (!route) {
    response.sendText(404, "{\"ok\":false,\"error\":\"页面不存在\"}");
  } else {
    try {
      route->handler(request, response);
    } catch (const std::exception& ex) {
      if (!response.started()) {
        response.sendText(500, "{\"ok\":false,\"error\":" + util::jsonQuote(ex.what()) + "}");
      }
    }
  }

  if (!response.started()) {
    response.sendText(500, "{\"ok\":false,\"error\":\"internal error\"}");
  }

#ifdef _WIN32
  shutdown(client, SD_SEND);
#else
  shutdown(client, SHUT_WR);
#endif
  net::closeSocket(client);
}
