#include "app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "applog.h"
#include "util.h"
#include "web_assets.h"

namespace {

constexpr size_t kMaxJsonBody = 1 << 20;
constexpr size_t kDownloadBuffer = 64 * 1024;

void sendJson(HttpResponse& res, int status, const std::string& body) {
  res.sendText(status, body, "application/json; charset=utf-8");
}

void sendError(HttpResponse& res, int status, const std::string& message) {
  sendJson(res, status, "{\"ok\":false,\"error\":" + util::jsonQuote(message) + "}");
}

bool readBodyFully(HttpRequest& req, std::string& out) {
  out.resize(static_cast<size_t>(req.contentLength));
  size_t offset = 0;
  while (offset < out.size()) {
    size_t got = req.readBody(&out[offset], out.size() - offset);
    if (got == 0) return false;
    offset += got;
  }
  return true;
}

bool validDeviceId(const std::string& id) {
  if (id.size() < 4 || id.size() > 64) return false;
  for (unsigned char c : id) {
    if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
  }
  return true;
}

std::string cleanDisplayName(const std::string& raw) {
  std::string trimmed = util::trim(raw);
  std::string out;
  for (unsigned char c : trimmed) {
    if (c < 0x20 || c == 0x7F) continue;
    out.push_back(static_cast<char>(c));
  }
  if (out.size() > 48) {
    size_t cut = 48;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out.empty() ? "设备" : out;
}

std::string fileJson(const FileEntry& file) {
  std::ostringstream out;
  out << "{\"id\":" << util::jsonQuote(file.id) << ",\"name\":" << util::jsonQuote(file.name)
      << ",\"relPath\":" << util::jsonQuote(file.relPath) << ",\"size\":" << file.size
      << ",\"sha256\":" << util::jsonQuote(file.sha256) << ",\"created\":" << file.created
      << ",\"owner\":" << util::jsonQuote(file.owner) << ",\"ownerName\":" << util::jsonQuote(file.ownerName)
      << "}";
  return out.str();
}

std::string deviceJson(const DeviceInfo& device) {
  std::ostringstream out;
  out << "{\"id\":" << util::jsonQuote(device.id) << ",\"name\":" << util::jsonQuote(device.name)
      << ",\"ip\":" << util::jsonQuote(device.ip) << ",\"platform\":" << util::jsonQuote(device.platform)
      << ",\"online\":" << (device.online ? "true" : "false") << ",\"firstSeen\":" << device.firstSeen
      << ",\"lastSeen\":" << device.lastSeen << "}";
  return out.str();
}

std::string transferJson(const TransferRecord& transfer) {
  std::ostringstream out;
  out << "{\"id\":" << util::jsonQuote(transfer.id) << ",\"direction\":" << util::jsonQuote(transfer.direction)
      << ",\"deviceId\":" << util::jsonQuote(transfer.deviceId)
      << ",\"deviceName\":" << util::jsonQuote(transfer.deviceName)
      << ",\"fileId\":" << util::jsonQuote(transfer.fileId) << ",\"fileName\":" << util::jsonQuote(transfer.fileName)
      << ",\"size\":" << transfer.size << ",\"bytes\":" << transfer.bytes
      << ",\"sha256\":" << util::jsonQuote(transfer.sha256) << ",\"status\":" << util::jsonQuote(transfer.status)
      << ",\"created\":" << transfer.created << "}";
  return out.str();
}

std::string requestJson(const FileRequest& request) {
  std::ostringstream out;
  out << "{\"id\":" << util::jsonQuote(request.id) << ",\"deviceId\":" << util::jsonQuote(request.deviceId)
      << ",\"deviceName\":" << util::jsonQuote(request.deviceName) << ",\"note\":" << util::jsonQuote(request.note)
      << ",\"status\":" << util::jsonQuote(request.status) << ",\"created\":" << request.created << "}";
  return out.str();
}

std::string contentDisposition(const std::string& name) {
  std::string ascii;
  ascii.reserve(name.size());
  for (unsigned char c : name) {
    if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') {
      ascii.push_back('_');
    } else {
      ascii.push_back(static_cast<char>(c));
    }
  }
  if (ascii.empty()) ascii = "download";
  return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + util::urlEncode(name);
}

std::string hostUrl(const std::string& host, int port) {
  std::string url = "http://" + host;
  if (port != 80) url += ":" + std::to_string(port);
  return url + "/";
}

std::string cookieValue(const HttpRequest& req, const std::string& name) {
  std::string cookie = req.header("cookie");
  size_t pos = 0;
  while (pos < cookie.size()) {
    size_t end = cookie.find(';', pos);
    std::string item = cookie.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    item = util::trim(item);
    size_t eq = item.find('=');
    if (eq != std::string::npos && item.substr(0, eq) == name) return item.substr(eq + 1);
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return "";
}

}  // namespace

App::App(AppOptions options) : options_(std::move(options)) {}

bool App::init(std::string& err) {
  if (!store_.init(options_.dataDir, options_.keepData, err)) return false;
  adminToken_ = util::randomHex(16);
  return true;
}

void App::setRuntime(int port, const std::string& localIp) {
  port_ = port;
  localIp_ = localIp;
}

void App::setFirewallReport(const std::string& report) {
  std::lock_guard<std::mutex> lock(firewallMutex_);
  firewallReport_ = report;
}

std::string App::adminUrl() const {
  return "http://127.0.0.1:" + std::to_string(port_) + "/admin?t=" + adminToken_;
}

std::string App::clientUrl() const { return hostUrl(options_.hostname, port_); }

std::string App::dataDirectory() const { return options_.dataDir; }

bool App::authorized(const HttpRequest& req) const {
  if (req.header("x-auth-token") == adminToken_) return true;
  if (req.param("t") == adminToken_) return true;
  return cookieValue(req, "fdadmin") == adminToken_;
}

bool App::requireDevice(HttpRequest& req, HttpResponse& res, std::string& deviceId) {
  deviceId = req.header("x-device-id");
  if (deviceId.empty()) deviceId = req.param("deviceId");
  std::string key = req.header("x-device-key");
  if (key.empty()) key = req.param("key");
  if (!validDeviceId(deviceId) || key.empty()) {
    sendError(res, 403, "设备标识无效，请刷新页面");
    return false;
  }
  if (!devices_.verifyKey(deviceId, key)) {
    sendError(res, 403, "设备密钥不匹配，请清除浏览器站点数据后重试");
    return false;
  }
  return true;
}

bool App::readJsonBody(HttpRequest& req, HttpResponse& res, util::JsonValue* out) {
  if (req.contentLength > kMaxJsonBody) {
    sendError(res, 413, "请求内容过大");
    return false;
  }
  std::string body;
  if (!readBodyFully(req, body)) {
    sendError(res, 400, "读取请求失败");
    return false;
  }
  util::JsonValue value;
  std::string err;
  if (!util::parseJson(body, value, err)) {
    sendError(res, 400, "JSON 解析失败: " + err);
    return false;
  }
  if (out) *out = std::move(value);
  return true;
}

void App::notifyAdmin(bool force) {
  int64_t now = util::nowMs();
  if (!force) {
    int64_t last = lastAdminNotify_.load();
    if (now - last < 250) return;
  }
  lastAdminNotify_.store(now);
  devices_.broadcastAdmins("state", "{}");
}

void App::notifyDevice(const std::string& deviceId) { devices_.sendTo(deviceId, "state", "{}"); }

void App::serveAsset(const std::string& key, HttpResponse& res) {
  auto& assets = webAssets();
  auto it = assets.find(key);
  if (it == assets.end()) {
    res.sendText(404, "404 Not Found", "text/plain; charset=utf-8");
    return;
  }
  res.addHeader("Cache-Control", "no-cache");
  res.begin(200, it->second.mime, it->second.size);
  res.write(it->second.data, it->second.size);
}

void App::handleIndex(HttpRequest&, HttpResponse& res) { serveAsset("/", res); }

void App::handleHealth(HttpRequest&, HttpResponse& res) {
  std::ostringstream out;
  out << "{\"ok\":true,\"app\":\"filedrop\",\"hostname\":" << util::jsonQuote(options_.hostname)
      << ",\"port\":" << port_ << ",\"ip\":" << util::jsonQuote(localIp_)
      << ",\"devices\":" << devices_.onlineCount() << ",\"dns\":" << (dnsActive_ ? "true" : "false") << "}";
  sendJson(res, 200, out.str());
}

void App::handleAdminPage(HttpRequest& req, HttpResponse& res) {
  std::string t = req.param("t");
  if (!t.empty() && t == adminToken_) {
    res.addHeader("Set-Cookie", "fdadmin=" + adminToken_ + "; Path=/; SameSite=Strict; Max-Age=31536000");
    res.addHeader("Location", "/admin");
    res.sendEmpty(302);
    return;
  }
  if (!authorized(req)) {
    res.sendText(403,
                 "<!doctype html><meta charset=\"utf-8\"><title>Filedrop</title>"
                 "<body style=\"background:#0b1120;color:#e6edf7;font-family:system-ui;padding:60px\">"
                 "<h1>需要管理权限</h1><p>请通过启动服务端时控制台打印的地址打开管理界面。</p></body>",
                 "text/html; charset=utf-8");
    return;
  }
  serveAsset("/admin", res);
}

void App::handleClientEvents(HttpRequest& req, HttpResponse& res) {
  std::string deviceId;
  if (!requireDevice(req, res, deviceId)) return;

  std::string name = req.param("name");
  if (!name.empty()) name = cleanDisplayName(name);
  std::string platform = req.param("platform");
  if (platform.size() > 120) platform.resize(120);

  res.addHeader("Cache-Control", "no-cache");
  if (!res.begin(200, "text/event-stream; charset=utf-8", net::kNoContentLength)) return;

  auto sink = std::make_shared<SseSink>(res.socket());
  devices_.attach(deviceId, name, req.remoteIp, platform, sink);
  sink->sendComment("connected");
  notifyAdmin(true);

  int ticks = 0;
  while (!stopping_ && !sink->closed()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (++ticks % 15 == 0) {
      if (!sink->sendComment("ping")) break;
    }
  }

  devices_.detach(deviceId, sink);
  sink->close();
  notifyAdmin(true);
}

void App::handleClientState(HttpRequest& req, HttpResponse& res) {
  std::string deviceId;
  if (!requireDevice(req, res, deviceId)) return;
  sendJson(res, 200, clientStateJson(deviceId));
}

void App::handleClientUpload(HttpRequest& req, HttpResponse& res) {
  std::string deviceId;
  if (!requireDevice(req, res, deviceId)) return;

  std::string fileName = util::urlDecode(req.header("x-file-name"));
  if (fileName.empty()) fileName = req.param("name");
  fileName = util::sanitizeName(fileName);
  if (fileName.empty()) {
    sendError(res, 400, "缺少文件名");
    return;
  }

  std::string relPath = util::normalizeRelPath(req.param("path", fileName));
  if (relPath.empty()) relPath = fileName;
  std::string batchId = req.param("batchId");
  if (batchId.empty()) batchId = util::randomHex(6);
  std::string requestId = req.param("requestId");

  std::string err;
  auto session = store_.beginUpload("device:" + deviceId, devices_.nameOf(deviceId), batchId, relPath, fileName, err);
  if (!session) {
    sendError(res, 500, err);
    return;
  }

  TransferRecord progress;
  progress.direction = "from-device";
  progress.deviceId = deviceId;
  progress.deviceName = devices_.nameOf(deviceId);
  progress.fileId = session->id;
  progress.fileName = fileName;
  progress.status = "上传中";
  progress.size = req.contentLength;
  std::string transferId = store_.addTransfer(progress);
  notifyAdmin(true);

  std::vector<char> buffer(kDownloadBuffer);
  uint64_t remaining = req.contentLength;
  int64_t lastNotify = util::nowMs();

  while (remaining > 0) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    size_t got = req.readBody(buffer.data(), want);
    if (got == 0) {
      store_.abortUpload(*session);
      store_.setTransferStatus(transferId, "已中断");
      notifyAdmin(true);
      sendError(res, 400, "上传中断");
      return;
    }
    if (!session->append(buffer.data(), got)) {
      store_.abortUpload(*session);
      store_.setTransferStatus(transferId, "写入失败");
      notifyAdmin(true);
      sendError(res, 500, "写入文件失败");
      return;
    }
    remaining -= got;
    int64_t now = util::nowMs();
    if (now - lastNotify > 300) {
      lastNotify = now;
      store_.setTransferBytes(transferId, session->size);
      notifyAdmin();
    }
  }

  FileEntry entry;
  if (!store_.finishUpload(*session, entry, err)) {
    store_.setTransferStatus(transferId, "写入失败");
    notifyAdmin(true);
    sendError(res, 500, err);
    return;
  }

  store_.setTransferBytes(transferId, entry.size);
  store_.setTransferStatus(transferId, "已完成");
  store_.setTransferDigest(transferId, entry.sha256);
  if (!requestId.empty()) markRequestFinished(requestId, deviceId, "已完成");
  notifyAdmin(true);
  notifyDevice(deviceId);

  sendJson(res, 200, "{\"ok\":true,\"file\":" + fileJson(entry) + "}");
}

void App::handleClientAck(HttpRequest& req, HttpResponse& res) {
  std::string deviceId;
  if (!requireDevice(req, res, deviceId)) return;

  util::JsonValue body;
  if (!readJsonBody(req, res, &body)) return;

  std::string transferId = body.stringOr("transferId");
  std::string requestId = body.stringOr("requestId");
  std::string status = body.stringOr("status");
  if (status != "已完成" && status != "已拒绝") {
    sendError(res, 400, "状态无效");
    return;
  }

  bool changed = false;
  if (!transferId.empty()) {
    auto transfer = store_.getTransfer(transferId);
    if (transfer && transfer->deviceId == deviceId) {
      store_.setTransferStatus(transferId, status);
      changed = true;
    }
  }
  if (!requestId.empty()) {
    markRequestFinished(requestId, deviceId, status);
    changed = true;
  }
  notifyAdmin(true);
  sendJson(res, 200, std::string("{\"ok\":true,\"changed\":") + (changed ? "true" : "false") + "}");
}

void App::handleDownload(HttpRequest& req, HttpResponse& res) {
  std::string tokenValue = req.path.substr(std::string("/dl/").size());
  if (tokenValue.empty()) {
    sendError(res, 404, "下载地址无效");
    return;
  }
  auto token = store_.getToken(tokenValue);
  if (!token) {
    sendError(res, 410, "下载链接已失效，请让服务器重新发送");
    return;
  }
  std::string deviceHeader = req.header("x-device-id");
  if (!deviceHeader.empty() && deviceHeader != token->deviceId) {
    sendError(res, 403, "该文件不属于当前设备");
    return;
  }
  auto file = store_.getFile(token->fileId);
  if (!file) {
    sendError(res, 410, "文件已被移除");
    return;
  }
  std::ifstream in(file->path, std::ios::binary);
  if (!in) {
    sendError(res, 410, "文件读取失败");
    return;
  }

  store_.setTransferStatus(token->transferId, "发送中");
  res.addHeader("Content-Disposition", contentDisposition(file->name));
  res.addHeader("Cache-Control", "no-store");
  if (!res.begin(200, "application/octet-stream", file->size)) return;

  std::vector<char> buffer(kDownloadBuffer);
  uint64_t sent = 0;
  int64_t lastNotify = util::nowMs();
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    std::streamsize got = in.gcount();
    if (got <= 0) break;
    if (!res.write(buffer.data(), static_cast<size_t>(got))) break;
    sent += static_cast<uint64_t>(got);
    int64_t now = util::nowMs();
    if (now - lastNotify > 300) {
      lastNotify = now;
      store_.setTransferBytes(token->transferId, sent);
      notifyAdmin();
    }
  }

  if (!res.broken() && sent == file->size) {
    store_.setTransferBytes(token->transferId, file->size);
    store_.setTransferStatus(token->transferId, "已完成");
  }
  notifyAdmin(true);
}

void App::handleAdminEvents(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  res.addHeader("Cache-Control", "no-cache");
  if (!res.begin(200, "text/event-stream; charset=utf-8", net::kNoContentLength)) return;

  auto sink = std::make_shared<SseSink>(res.socket());
  devices_.addAdminSink(sink);
  sink->sendComment("connected");

  int ticks = 0;
  while (!stopping_ && !sink->closed()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (++ticks % 15 == 0) {
      if (!sink->sendComment("ping")) break;
    }
  }
  devices_.removeAdminSink(sink);
  sink->close();
}

void App::handleAdminState(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  sendJson(res, 200, adminStateJson());
}

void App::handleAdminLog(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  res.addHeader("Cache-Control", "no-store");
  res.sendText(200, applog::tail(200), "text/plain; charset=utf-8");
}

void App::handleAdminUpload(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  std::string fileName = util::urlDecode(req.header("x-file-name"));
  if (fileName.empty()) fileName = req.param("name");
  fileName = util::sanitizeName(fileName);
  if (fileName.empty()) {
    sendError(res, 400, "缺少文件名");
    return;
  }
  std::string relPath = util::normalizeRelPath(req.param("path", fileName));
  if (relPath.empty()) relPath = fileName;
  std::string batchId = req.param("batchId");
  if (batchId.empty()) batchId = util::randomHex(6);

  std::string err;
  auto session = store_.beginUpload("server", "服务端", batchId, relPath, fileName, err);
  if (!session) {
    sendError(res, 500, err);
    return;
  }

  std::vector<char> buffer(kDownloadBuffer);
  uint64_t remaining = req.contentLength;
  int64_t lastNotify = util::nowMs();
  while (remaining > 0) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    size_t got = req.readBody(buffer.data(), want);
    if (got == 0) {
      store_.abortUpload(*session);
      sendError(res, 400, "上传中断");
      return;
    }
    if (!session->append(buffer.data(), got)) {
      store_.abortUpload(*session);
      sendError(res, 500, "写入文件失败");
      return;
    }
    remaining -= got;
    if (util::nowMs() - lastNotify > 400) {
      lastNotify = util::nowMs();
      notifyAdmin();
    }
  }

  FileEntry entry;
  if (!store_.finishUpload(*session, entry, err)) {
    sendError(res, 500, err);
    return;
  }
  notifyAdmin(true);
  sendJson(res, 200, "{\"ok\":true,\"file\":" + fileJson(entry) + "}");
}

void App::handleAdminSend(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  util::JsonValue body;
  if (!readJsonBody(req, res, &body)) return;

  auto deviceIds = body.stringArray("deviceIds");
  auto fileIds = body.stringArray("fileIds");
  if (deviceIds.empty() || fileIds.empty()) {
    sendError(res, 400, "请选择设备和文件");
    return;
  }

  std::string batchId = util::randomHex(6);
  size_t sent = 0;
  for (const auto& deviceId : deviceIds) {
    if (!devices_.exists(deviceId)) continue;
    std::string deviceName = devices_.nameOf(deviceId);
    bool any = false;
    for (const auto& fileId : fileIds) {
      auto file = store_.getFile(fileId);
      if (!file) continue;
      TransferRecord transfer;
      transfer.direction = "to-device";
      transfer.deviceId = deviceId;
      transfer.deviceName = deviceName;
      transfer.fileId = file->id;
      transfer.fileName = file->name;
      transfer.size = file->size;
      transfer.sha256 = file->sha256;
      transfer.status = "待接收";
      std::string transferId = store_.addTransfer(transfer);
      store_.issueToken(file->id, deviceId, transferId, batchId);
      ++sent;
      any = true;
    }
    if (any) notifyDevice(deviceId);
  }

  if (sent == 0) {
    sendError(res, 400, "没有可发送的文件或设备");
    return;
  }
  notifyAdmin(true);
  sendJson(res, 200, "{\"ok\":true,\"sent\":" + std::to_string(sent) + ",\"batchId\":" + util::jsonQuote(batchId) +
                          "}");
}

void App::handleAdminRequest(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  util::JsonValue body;
  if (!readJsonBody(req, res, &body)) return;

  auto deviceIds = body.stringArray("deviceIds");
  std::string note = body.stringOr("note");
  if (deviceIds.empty()) {
    sendError(res, 400, "请选择设备");
    return;
  }
  note = util::trim(note);
  if (note.size() > 120) note.resize(120);

  size_t created = 0;
  {
    std::lock_guard<std::mutex> lock(requestsMutex_);
    for (const auto& deviceId : deviceIds) {
      if (!devices_.exists(deviceId)) continue;
      FileRequest request;
      request.id = util::randomHex(8);
      request.deviceId = deviceId;
      request.deviceName = devices_.nameOf(deviceId);
      request.note = note.empty() ? "服务器请求你发送文件" : note;
      request.status = "待发送";
      request.created = util::nowMs();
      requests_.push_back(request);
      ++created;
    }
    if (requests_.size() > 100) {
      requests_.erase(requests_.begin(), requests_.begin() + static_cast<std::ptrdiff_t>(requests_.size() - 100));
    }
  }

  if (created == 0) {
    sendError(res, 400, "设备不可用");
    return;
  }
  for (const auto& deviceId : deviceIds) notifyDevice(deviceId);
  notifyAdmin(true);
  sendJson(res, 200, "{\"ok\":true,\"created\":" + std::to_string(created) + "}");
}

void App::handleAdminRename(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  util::JsonValue body;
  if (!readJsonBody(req, res, &body)) return;

  std::string deviceId = body.stringOr("deviceId");
  std::string name = cleanDisplayName(body.stringOr("name"));
  if (deviceId.empty() || !devices_.rename(deviceId, name)) {
    sendError(res, 404, "设备不存在");
    return;
  }
  notifyDevice(deviceId);
  notifyAdmin(true);
  sendJson(res, 200, "{\"ok\":true}");
}

void App::handleAdminRemove(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  util::JsonValue body;
  if (!readJsonBody(req, res, &body)) return;

  std::string fileId = body.stringOr("fileId");
  bool removed = store_.removeFile(fileId);
  notifyAdmin(true);
  sendJson(res, 200, std::string("{\"ok\":true,\"removed\":") + (removed ? "true" : "false") + "}");
}

void App::handleAdminClearHistory(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  store_.clearHistory();
  notifyAdmin(true);
  sendJson(res, 200, "{\"ok\":true}");
}

void App::handleAdminDownload(HttpRequest& req, HttpResponse& res) {
  if (!authorized(req)) {
    sendError(res, 403, "需要管理权限");
    return;
  }
  std::string fileId = req.path.substr(std::string("/api/admin/download/").size());
  auto file = store_.getFile(fileId);
  if (!file) {
    sendError(res, 404, "文件不存在");
    return;
  }
  std::ifstream in(file->path, std::ios::binary);
  if (!in) {
    sendError(res, 410, "文件已被移除");
    return;
  }

  res.addHeader("Content-Disposition", contentDisposition(file->name));
  res.addHeader("Cache-Control", "no-store");
  if (!res.begin(200, "application/octet-stream", file->size)) return;

  std::vector<char> buffer(kDownloadBuffer);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    std::streamsize got = in.gcount();
    if (got <= 0) break;
    if (!res.write(buffer.data(), static_cast<size_t>(got))) break;
  }
}

void App::markRequestFinished(const std::string& requestId, const std::string& deviceId,
                              const std::string& status) {
  std::lock_guard<std::mutex> lock(requestsMutex_);
  for (auto& request : requests_) {
    if (request.id == requestId && request.deviceId == deviceId) {
      request.status = status;
      return;
    }
  }
}

std::string App::firewallSnapshot() const {
  std::lock_guard<std::mutex> lock(firewallMutex_);
  return firewallReport_;
}

std::string App::adminStateJson() {
  std::ostringstream out;
  out << "{\"ok\":true,\"server\":{"
      << "\"hostname\":" << util::jsonQuote(options_.hostname) << ",\"port\":" << port_
      << ",\"ip\":" << util::jsonQuote(localIp_) << ",\"clientUrl\":" << util::jsonQuote(clientUrl())
      << ",\"clientUrlIp\":" << util::jsonQuote(hostUrl(localIp_, port_))
      << ",\"dataDir\":" << util::jsonQuote(options_.dataDir) << ",\"dns\":" << (dnsActive_ ? "true" : "false")
      << ",\"firewall\":" << util::jsonQuote(firewallSnapshot());

  out << ",\"aliases\":[";
  for (size_t i = 0; i < options_.aliases.size(); ++i) {
    if (i) out << ",";
    out << util::jsonQuote(options_.aliases[i]);
  }
  out << "]";

  auto interfaces = util::localInterfaces();
  out << ",\"urls\":[";
  size_t index = 0;
  for (const auto& info : interfaces) {
    if (index) out << ",";
    out << "{\"ip\":" << util::jsonQuote(info.ip) << ",\"name\":" << util::jsonQuote(info.name)
        << ",\"netmask\":" << util::jsonQuote(info.netmask)
        << ",\"primary\":" << (info.ip == localIp_ ? "true" : "false")
        << ",\"url\":" << util::jsonQuote(hostUrl(info.ip, port_)) << "}";
    ++index;
  }
  out << "]";
  out << "}";

  auto devices = devices_.snapshot();
  std::sort(devices.begin(), devices.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
    if (a.online != b.online) return a.online > b.online;
    return a.lastSeen > b.lastSeen;
  });
  out << ",\"devices\":[";
  for (size_t i = 0; i < devices.size(); ++i) {
    if (i) out << ",";
    out << deviceJson(devices[i]);
  }
  out << "]";

  auto staging = store_.filesOwnedBy("server");
  std::sort(staging.begin(), staging.end(), [](const FileEntry& a, const FileEntry& b) { return a.created > b.created; });
  out << ",\"staging\":[";
  for (size_t i = 0; i < staging.size(); ++i) {
    if (i) out << ",";
    out << fileJson(staging[i]);
  }
  out << "]";

  auto received = store_.receivedFiles();
  out << ",\"received\":[";
  for (size_t i = 0; i < received.size(); ++i) {
    if (i) out << ",";
    out << fileJson(received[i]);
  }
  out << "]";

  auto transfers = store_.transfers();
  std::reverse(transfers.begin(), transfers.end());
  out << ",\"transfers\":[";
  size_t transferCount = std::min<size_t>(transfers.size(), 200);
  for (size_t i = 0; i < transferCount; ++i) {
    if (i) out << ",";
    out << transferJson(transfers[i]);
  }
  out << "]";

  std::vector<FileRequest> requests;
  {
    std::lock_guard<std::mutex> lock(requestsMutex_);
    requests = requests_;
  }
  std::sort(requests.begin(), requests.end(), [](const FileRequest& a, const FileRequest& b) {
    return a.created > b.created;
  });
  out << ",\"requests\":[";
  size_t requestCount = std::min<size_t>(requests.size(), 50);
  for (size_t i = 0; i < requestCount; ++i) {
    if (i) out << ",";
    out << requestJson(requests[i]);
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string App::clientStateJson(const std::string& deviceId) {
  std::ostringstream out;
  out << "{\"ok\":true,\"device\":{\"id\":" << util::jsonQuote(deviceId)
      << ",\"name\":" << util::jsonQuote(devices_.nameOf(deviceId))
      << "},\"server\":{\"hostname\":" << util::jsonQuote(options_.hostname) << ",\"port\":" << port_ << "}";

  struct Group {
    std::string batchId;
    int64_t created = 0;
    std::string items;
  };
  std::vector<Group> groups;
  for (const auto& token : store_.tokensForDevice(deviceId)) {
    auto transfer = store_.getTransfer(token.transferId);
    if (!transfer) continue;
    if (transfer->status == "已完成" || transfer->status == "已拒绝") continue;
    auto file = store_.getFile(token.fileId);
    if (!file) continue;

    std::ostringstream item;
    item << "{\"transferId\":" << util::jsonQuote(transfer->id) << ",\"fileId\":" << util::jsonQuote(file->id)
         << ",\"name\":" << util::jsonQuote(file->name) << ",\"relPath\":" << util::jsonQuote(file->relPath)
         << ",\"size\":" << file->size << ",\"sha256\":" << util::jsonQuote(file->sha256)
         << ",\"url\":" << util::jsonQuote("/dl/" + token.token)
         << ",\"status\":" << util::jsonQuote(transfer->status) << "}";

    auto it = std::find_if(groups.begin(), groups.end(),
                           [&](const Group& group) { return group.batchId == token.batchId; });
    if (it == groups.end()) {
      Group group;
      group.batchId = token.batchId;
      group.created = transfer->created;
      group.items = item.str();
      groups.push_back(std::move(group));
    } else {
      it->items += "," + item.str();
      it->created = std::max(it->created, transfer->created);
    }
  }

  out << ",\"offers\":[";
  for (size_t i = 0; i < groups.size(); ++i) {
    if (i) out << ",";
    out << "{\"batchId\":" << util::jsonQuote(groups[i].batchId) << ",\"created\":" << groups[i].created
        << ",\"files\":[" << groups[i].items << "]}";
  }
  out << "]";

  std::vector<FileRequest> requests;
  {
    std::lock_guard<std::mutex> lock(requestsMutex_);
    for (const auto& request : requests_) {
      if (request.deviceId == deviceId && request.status == "待发送") requests.push_back(request);
    }
  }
  out << ",\"requests\":[";
  for (size_t i = 0; i < requests.size(); ++i) {
    if (i) out << ",";
    out << requestJson(requests[i]);
  }
  out << "]";

  auto history = store_.transfersForDevice(deviceId);
  std::reverse(history.begin(), history.end());
  out << ",\"history\":[";
  size_t historyCount = std::min<size_t>(history.size(), 60);
  for (size_t i = 0; i < historyCount; ++i) {
    if (i) out << ",";
    out << transferJson(history[i]);
  }
  out << "]";

  out << "}";
  return out.str();
}

void App::registerRoutes(HttpServer& server) {
  server.get("/", [this](HttpRequest& req, HttpResponse& res) { handleIndex(req, res); });
  server.get("/health", [this](HttpRequest& req, HttpResponse& res) { handleHealth(req, res); });
  server.get("/admin", [this](HttpRequest& req, HttpResponse& res) { handleAdminPage(req, res); });

  // 其余内嵌资源(样式、脚本、图标等)按实际打包内容自动注册。
  for (const auto& asset : webAssets()) {
    const std::string key = asset.first;
    if (key == "/" || key == "/admin") continue;
    server.get(key, [this, key](HttpRequest&, HttpResponse& res) { serveAsset(key, res); });
  }

  server.get("/api/client/state",
             [this](HttpRequest& req, HttpResponse& res) { handleClientState(req, res); });
  server.get("/api/client/events",
             [this](HttpRequest& req, HttpResponse& res) { handleClientEvents(req, res); });
  server.put("/api/client/upload",
             [this](HttpRequest& req, HttpResponse& res) { handleClientUpload(req, res); });
  server.post("/api/client/ack",
              [this](HttpRequest& req, HttpResponse& res) { handleClientAck(req, res); });
  server.onPrefix("GET", "/dl/", [this](HttpRequest& req, HttpResponse& res) { handleDownload(req, res); });

  server.get("/api/admin/state",
             [this](HttpRequest& req, HttpResponse& res) { handleAdminState(req, res); });
  server.get("/api/admin/log", [this](HttpRequest& req, HttpResponse& res) { handleAdminLog(req, res); });
  server.get("/api/admin/events",
             [this](HttpRequest& req, HttpResponse& res) { handleAdminEvents(req, res); });
  server.put("/api/admin/upload",
             [this](HttpRequest& req, HttpResponse& res) { handleAdminUpload(req, res); });
  server.post("/api/admin/send",
              [this](HttpRequest& req, HttpResponse& res) { handleAdminSend(req, res); });
  server.post("/api/admin/request",
              [this](HttpRequest& req, HttpResponse& res) { handleAdminRequest(req, res); });
  server.post("/api/admin/rename",
              [this](HttpRequest& req, HttpResponse& res) { handleAdminRename(req, res); });
  server.post("/api/admin/remove",
              [this](HttpRequest& req, HttpResponse& res) { handleAdminRemove(req, res); });
  server.post("/api/admin/clear-history",
              [this](HttpRequest& req, HttpResponse& res) { handleAdminClearHistory(req, res); });
  server.onPrefix("GET", "/api/admin/download/",
                  [this](HttpRequest& req, HttpResponse& res) { handleAdminDownload(req, res); });
}
