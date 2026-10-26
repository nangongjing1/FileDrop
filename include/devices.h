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
  // 读取数据目录中的 config.ini，恢复设备的重命名记录。
  void loadNames(const std::string& configPath);
  bool refreshNames();

  bool verifyKey(const std::string& id, const std::string& key);
  void attach(const std::string& id, const std::string& ip, const std::string& platform,
              std::shared_ptr<SseSink> sink);
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
  void parseNamesLocked(const std::string& content);
  void applySavedNamesLocked();
  bool reloadNamesIfChangedLocked();
  // 把当前重命名记录写回 config.ini；调用方需持有 mutex_。
  bool saveNamesLocked();

  std::mutex mutex_;
  std::map<std::string, DeviceInfo> devices_;
  std::map<std::string, std::string> savedNames_;
  std::string configPath_;
  std::string configRaw_;
  std::vector<std::shared_ptr<SseSink>> admins_;
};
