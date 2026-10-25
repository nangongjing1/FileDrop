#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int socklen_type;
static const socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <netinet/in.h>
#include <sys/socket.h>
typedef int socket_t;
typedef socklen_t socklen_type;
static const socket_t kInvalidSocket = -1;
#endif

namespace net {

constexpr uint64_t kNoContentLength = ~static_cast<uint64_t>(0);

bool initSockets(std::string& err);
void cleanupSockets();
bool setRecvTimeout(socket_t s, int ms);
bool setSendTimeout(socket_t s, int ms);
void closeSocket(socket_t s);
bool sendAll(socket_t s, const void* data, size_t len);
bool wouldBlock();

}  // namespace net

struct HttpRequest {
  std::string method;
  std::string target;
  std::string path;
  std::string query;
  std::string remoteIp;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> params;
  uint64_t contentLength = 0;
  std::function<size_t(char*, size_t)> readBody;

  std::string header(const std::string& lowerName) const;
  std::string param(const std::string& key, const std::string& def = "") const;
};

class HttpResponse {
 public:
  explicit HttpResponse(socket_t socket);

  void addHeader(const std::string& name, const std::string& value);
  bool begin(int status, const std::string& contentType, uint64_t contentLength);
  bool write(const void* data, size_t len);
  bool write(const std::string& data);
  bool sendText(int status, const std::string& body,
                const std::string& contentType = "application/json; charset=utf-8");
  bool sendEmpty(int status);
  bool started() const { return started_; }
  bool broken() const { return broken_; }
  uint64_t written() const { return written_; }
  socket_t socket() const { return socket_; }

 private:
  socket_t socket_;
  std::vector<std::pair<std::string, std::string>> headers_;
  bool started_ = false;
  bool broken_ = false;
  uint64_t expected_ = 0;
  uint64_t written_ = 0;
};

class HttpServer {
 public:
  using Handler = std::function<void(HttpRequest&, HttpResponse&)>;

  HttpServer() = default;
  ~HttpServer();

  void get(const std::string& path, Handler handler);
  void post(const std::string& path, Handler handler);
  void put(const std::string& path, Handler handler);
  void onPrefix(const std::string& method, const std::string& prefix, Handler handler);

  bool start(const std::string& bindAddress, int port, std::string& err);
  void stop();
  int port() const { return port_; }
  int activeConnections() const { return activeConnections_; }

 private:
  struct Route {
    std::string method;
    std::string path;
    bool prefix = false;
    Handler handler;
  };

  void acceptLoop();
  void handleConnection(socket_t client, std::string remoteIp);
  const Route* matchRoute(const std::string& method, const std::string& path) const;

  std::vector<Route> routes_;
  socket_t listenSocket_ = kInvalidSocket;
  std::thread acceptThread_;
  std::atomic<bool> running_{false};
  std::atomic<int> activeConnections_{0};
  int port_ = 0;
};
