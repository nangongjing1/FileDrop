#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "devices.h"
#include "http_server.h"
#include "store.h"
#include "util.h"

struct AppOptions {
  std::string hostname = "filedrop.ng";
  std::vector<std::string> aliases;
  std::string dataDir = "filedrop-data";
  std::string bindAddress = "0.0.0.0";
  int port = 80;
  bool keepData = false;
};

struct FileRequest {
  std::string id;
  std::string deviceId;
  std::string deviceName;
  std::string note;
  std::string status;
  int64_t created = 0;
};

class App {
 public:
  explicit App(AppOptions options);

  bool init(std::string& err);
  void registerRoutes(HttpServer& server);
  void setRuntime(int port, const std::string& localIp);
  void setDnsActive(bool active) { dnsActive_ = active; }
  void setFirewallReport(const std::string& report);
  void stop() { stopping_ = true; }

  std::string adminToken() const { return adminToken_; }
  std::string adminUrl() const;
  std::string clientUrl() const;
  std::string localIp() const { return localIp_; }
  std::string firewallSnapshot() const;
  int port() const { return port_; }
  std::string dataDirectory() const;

 private:
  void serveAsset(const std::string& key, HttpResponse& res);
  void handleIndex(HttpRequest& req, HttpResponse& res);
  void handleHealth(HttpRequest& req, HttpResponse& res);
  void handleAdminPage(HttpRequest& req, HttpResponse& res);
  void handleClientEvents(HttpRequest& req, HttpResponse& res);
  void handleClientState(HttpRequest& req, HttpResponse& res);
  void handleClientUpload(HttpRequest& req, HttpResponse& res);
  void handleClientAck(HttpRequest& req, HttpResponse& res);
  void handleDownload(HttpRequest& req, HttpResponse& res);
  void handleAdminState(HttpRequest& req, HttpResponse& res);
  void handleAdminLog(HttpRequest& req, HttpResponse& res);
  void handleAdminEvents(HttpRequest& req, HttpResponse& res);
  void handleAdminUpload(HttpRequest& req, HttpResponse& res);
  void handleAdminSend(HttpRequest& req, HttpResponse& res);
  void handleAdminRequest(HttpRequest& req, HttpResponse& res);
  void handleAdminRename(HttpRequest& req, HttpResponse& res);
  void handleAdminRemove(HttpRequest& req, HttpResponse& res);
  void handleAdminClearHistory(HttpRequest& req, HttpResponse& res);
  void handleAdminDownload(HttpRequest& req, HttpResponse& res);

  bool authorized(const HttpRequest& req) const;
  bool requireDevice(HttpRequest& req, HttpResponse& res, std::string& deviceId);
  bool readJsonBody(HttpRequest& req, HttpResponse& res, util::JsonValue* out);
  void notifyAdmin(bool force = false);
  void notifyDevice(const std::string& deviceId);
  void markRequestFinished(const std::string& requestId, const std::string& deviceId, const std::string& status);

  std::string adminStateJson();
  std::string clientStateJson(const std::string& deviceId);

  AppOptions options_;
  std::string adminToken_;
  std::string localIp_;
  mutable std::mutex firewallMutex_;
  std::string firewallReport_;
  int port_ = 80;
  std::atomic<bool> dnsActive_{false};

  FileStore store_;
  DeviceRegistry devices_;

  std::mutex requestsMutex_;
  std::vector<FileRequest> requests_;

  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> lastAdminNotify_{0};
};
