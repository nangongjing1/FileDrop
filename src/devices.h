#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "http_server.h"

class SseSink {
 public:
  explicit SseSink(socket_t socket);

  bool sendEvent(const std::string& event, const std::string& data);
  bool sendComment(const std::string& text);
  void close();
  bool closed() const;

 private:
  bool sendFrame(const std::string& frame);

  mutable std::mutex mutex_;
  socket_t socket_;
  bool closed_ = false;
};

struct DeviceInfo {
  std::string id;
  std::string key;
  std::string name;
  std::string ip;
  std::string platform;
  bool online = false;
  int64_t firstSeen = 0;
  int64_t lastSeen = 0;
  std::shared_ptr<SseSink> sink;
};

class DeviceRegistry {
 public:
  bool verifyKey(const std::string& id, const std::string& key);
  void attach(const std::string& id, const std::string& name, const std::string& ip,
              const std::string& platform, std::shared_ptr<SseSink> sink);
  void detach(const std::string& id, const std::shared_ptr<SseSink>& sink);
  bool rename(const std::string& id, const std::string& name);
  std::string nameOf(const std::string& id);
  bool exists(const std::string& id);
  bool sendTo(const std::string& id, const std::string& event, const std::string& data);
  std::vector<DeviceInfo> snapshot();
  size_t onlineCount();

  void addAdminSink(const std::shared_ptr<SseSink>& sink);
  void removeAdminSink(const std::shared_ptr<SseSink>& sink);
  void broadcastAdmins(const std::string& event, const std::string& data);

 private:
  std::mutex mutex_;
  std::map<std::string, DeviceInfo> devices_;
  std::vector<std::shared_ptr<SseSink>> admins_;
};
