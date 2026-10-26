#include "devices.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "applog.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kDevicesSection = "[devices]";

std::string defaultDeviceName(const std::string& id) {
  return "设备-" + id.substr(0, std::min<size_t>(4, id.size()));
}

std::string readConfigFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

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

void DeviceRegistry::loadNames(const std::string& configPath) {
  std::lock_guard<std::mutex> lock(mutex_);
  configPath_ = configPath;
  configRaw_ = readConfigFile(configPath_);
  parseNamesLocked(configRaw_);
  applySavedNamesLocked();
}

bool DeviceRegistry::refreshNames() {
  std::lock_guard<std::mutex> lock(mutex_);
  return reloadNamesIfChangedLocked();
}

void DeviceRegistry::parseNamesLocked(const std::string& content) {
  savedNames_.clear();

  std::istringstream in(content);
  bool inDevices = false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string text = util::trim(line);
    if (text.empty() || text[0] == ';' || text[0] == '#') continue;
    if (text[0] == '[') {
      inDevices = text == kDevicesSection;
      continue;
    }
    if (!inDevices) continue;
    size_t eq = text.find('=');
    if (eq == std::string::npos) continue;
    std::string id = util::trim(text.substr(0, eq));
    std::string name = util::trim(text.substr(eq + 1));
    if (id.empty() || name.empty()) continue;
    savedNames_[id] = name;
  }
}

bool DeviceRegistry::reloadNamesIfChangedLocked() {
  if (configPath_.empty()) return false;
  std::string raw = readConfigFile(configPath_);
  if (raw == configRaw_) return false;
  configRaw_ = std::move(raw);
  parseNamesLocked(configRaw_);
  applySavedNamesLocked();
  return true;
}

void DeviceRegistry::applySavedNamesLocked() {
  for (auto& kv : devices_) {
    auto saved = savedNames_.find(kv.first);
    kv.second.name = saved != savedNames_.end() ? saved->second : defaultDeviceName(kv.first);
  }
}

bool DeviceRegistry::saveNamesLocked() {
  if (configPath_.empty()) return false;

  std::string kept;
  {
    std::ifstream in(configPath_, std::ios::binary);
    bool inDevices = false;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string text = util::trim(line);
      if (!text.empty() && text[0] == '[') {
        inDevices = text == kDevicesSection;
        if (inDevices) continue;
      } else if (inDevices) {
        continue;
      }
      kept += line;
      kept += '\n';
    }
  }
  while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r')) kept.pop_back();

  std::string text = kept;
  if (text.empty()) {
    text = "; Filedrop 配置文件\n; 设备重命名记录保存在 [devices] 中，删除对应行即可恢复默认名称。";
  }
  text += "\n\n";
  text += kDevicesSection;
  text += '\n';
  for (const auto& kv : savedNames_) {
    text += kv.first;
    text += '=';
    text += kv.second;
    text += '\n';
  }

  std::error_code ec;
  const std::string tmpPath = configPath_ + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    out.flush();
    if (!out) {
      out.close();
      fs::remove(tmpPath, ec);
      return false;
    }
  }
  fs::remove(configPath_, ec);
  ec.clear();
  fs::rename(tmpPath, configPath_, ec);
  if (ec) {
    applog::info("写入配置失败: " + ec.message());
    fs::remove(tmpPath, ec);
    return false;
  }
  configRaw_ = text;
  return true;
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

void DeviceRegistry::attach(const std::string& id, const std::string& ip,
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
  applySavedNamesLocked();
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

  auto previous = savedNames_.find(id);
  const bool hadPrevious = previous != savedNames_.end();
  const std::string previousName = hadPrevious ? previous->second : std::string();
  savedNames_[id] = name;
  if (!saveNamesLocked()) {
    if (hadPrevious) {
      savedNames_[id] = previousName;
    } else {
      savedNames_.erase(id);
    }
    applySavedNamesLocked();
    return false;
  }
  applySavedNamesLocked();
  return true;
}

std::string DeviceRegistry::nameOf(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  reloadNamesIfChangedLocked();
  auto it = devices_.find(id);
  if (it != devices_.end() && !it->second.name.empty()) return it->second.name;
  auto saved = savedNames_.find(id);
  if (saved != savedNames_.end()) return saved->second;
  return defaultDeviceName(id);
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
  reloadNamesIfChangedLocked();
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
