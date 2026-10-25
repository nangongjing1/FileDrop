#include "devices.h"

#include <algorithm>

#include "util.h"

SseSink::SseSink(socket_t socket) : socket_(socket) {}

bool SseSink::sendFrame(const std::string& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return false;
  if (!net::sendAll(socket_, frame.data(), frame.size())) {
    closed_ = true;
    return false;
  }
  return true;
}

bool SseSink::sendEvent(const std::string& event, const std::string& data) {
  std::string frame = "event: " + event + "\ndata: " + data + "\n\n";
  return sendFrame(frame);
}

bool SseSink::sendComment(const std::string& text) { return sendFrame(": " + text + "\n\n"); }

void SseSink::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
}

bool SseSink::closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

bool DeviceRegistry::verifyKey(const std::string& id, const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = devices_.find(id);
  if (it == devices_.end()) {
    DeviceInfo device;
    device.id = id;
    device.key = key;
    device.firstSeen = util::nowMs();
    device.lastSeen = device.firstSeen;
    device.online = false;
    devices_.emplace(id, std::move(device));
    return true;
  }
  if (it->second.key.empty()) {
    it->second.key = key;
    return true;
  }
  return it->second.key == key;
}

void DeviceRegistry::attach(const std::string& id, const std::string& name, const std::string& ip,
                            const std::string& platform, std::shared_ptr<SseSink> sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = devices_.find(id);
  if (it == devices_.end()) {
    DeviceInfo device;
    device.id = id;
    device.firstSeen = util::nowMs();
    it = devices_.emplace(id, std::move(device)).first;
  }
  DeviceInfo& device = it->second;
  if (device.sink && device.sink != sink) {
    device.sink->close();
  }
  if (!name.empty()) device.name = name;
  if (device.name.empty()) device.name = "设备-" + id.substr(0, std::min<size_t>(4, id.size()));
  if (!ip.empty()) device.ip = ip;
  if (!platform.empty()) device.platform = platform;
  device.sink = std::move(sink);
  device.online = true;
  device.lastSeen = util::nowMs();
}

void DeviceRegistry::detach(const std::string& id, const std::shared_ptr<SseSink>& sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = devices_.find(id);
  if (it == devices_.end()) return;
  if (it->second.sink != sink) return;
  it->second.sink.reset();
  it->second.online = false;
  it->second.lastSeen = util::nowMs();
}

bool DeviceRegistry::rename(const std::string& id, const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = devices_.find(id);
  if (it == devices_.end()) return false;
  it->second.name = name;
  return true;
}

std::string DeviceRegistry::nameOf(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = devices_.find(id);
  if (it == devices_.end()) return "设备-" + id.substr(0, std::min<size_t>(4, id.size()));
  return it->second.name;
}

bool DeviceRegistry::exists(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_.find(id) != devices_.end();
}

bool DeviceRegistry::sendTo(const std::string& id, const std::string& event, const std::string& data) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = devices_.find(id);
  if (it == devices_.end() || !it->second.sink) return false;
  return it->second.sink->sendEvent(event, data);
}

std::vector<DeviceInfo> DeviceRegistry::snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DeviceInfo> out;
  out.reserve(devices_.size());
  for (auto& kv : devices_) {
    DeviceInfo copy = kv.second;
    copy.sink.reset();
    out.push_back(std::move(copy));
  }
  return out;
}

size_t DeviceRegistry::onlineCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(std::count_if(devices_.begin(), devices_.end(),
                                           [](const auto& kv) { return kv.second.online; }));
}

void DeviceRegistry::addAdminSink(const std::shared_ptr<SseSink>& sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  admins_.push_back(sink);
}

void DeviceRegistry::removeAdminSink(const std::shared_ptr<SseSink>& sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  admins_.erase(std::remove(admins_.begin(), admins_.end(), sink), admins_.end());
}

void DeviceRegistry::broadcastAdmins(const std::string& event, const std::string& data) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = admins_.begin(); it != admins_.end();) {
    if (!(*it)->sendEvent(event, data)) {
      it = admins_.erase(it);
    } else {
      ++it;
    }
  }
}
