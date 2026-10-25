#include "store.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "util.h"

namespace fs = std::filesystem;

namespace {
constexpr int64_t kTokenTtlMs = 10 * 60 * 1000;
constexpr size_t kMaxHistory = 500;
}  // namespace

bool UploadSession::append(const char* data, size_t len) {
  if (len == 0) return true;
  out.write(data, static_cast<std::streamsize>(len));
  if (!out) return false;
  sha.update(data, len);
  size += len;
  return true;
}

bool UploadSession::finish() {
  out.flush();
  bool good = static_cast<bool>(out);
  out.close();
  return good;
}

bool FileStore::init(const std::string& directory, bool keepExisting, std::string& err) {
  directory_ = directory;
  filesDir_ = (fs::path(directory_) / "files").string();
  tmpDir_ = (fs::path(directory_) / "tmp").string();

  std::error_code ec;
  if (!keepExisting) {
    fs::remove_all(filesDir_, ec);
    fs::remove_all(tmpDir_, ec);
  }
  fs::create_directories(filesDir_, ec);
  if (ec) {
    err = "无法创建数据目录: " + ec.message();
    return false;
  }
  fs::create_directories(tmpDir_, ec);
  if (ec) {
    err = "无法创建临时目录: " + ec.message();
    return false;
  }
  return true;
}

std::unique_ptr<UploadSession> FileStore::beginUpload(const std::string& owner, const std::string& ownerName,
                                                      const std::string& batchId, const std::string& relPath,
                                                      const std::string& name, std::string& err) {
  auto session = std::make_unique<UploadSession>();
  session->id = util::randomHex(12);
  session->name = name;
  session->relPath = relPath.empty() ? name : relPath;
  session->owner = owner;
  session->ownerName = ownerName;
  session->batchId = batchId;
  session->tmpPath = (fs::path(tmpDir_) / (session->id + ".part")).string();
  session->out.open(session->tmpPath, std::ios::binary | std::ios::trunc);
  if (!session->out) {
    err = "无法创建临时文件";
    return nullptr;
  }
  return session;
}

bool FileStore::finishUpload(UploadSession& session, FileEntry& entry, std::string& err) {
  if (!session.finish()) {
    err = "写入文件失败";
    return false;
  }

  std::string finalPath = (fs::path(filesDir_) / session.id).string();
  std::error_code ec;
  fs::rename(session.tmpPath, finalPath, ec);
  if (ec) {
    fs::remove(session.tmpPath, ec);
    err = "保存文件失败: " + ec.message();
    return false;
  }

  entry.id = session.id;
  entry.name = session.name;
  entry.relPath = session.relPath;
  entry.owner = session.owner;
  entry.ownerName = session.ownerName;
  entry.batchId = session.batchId;
  entry.path = finalPath;
  entry.size = session.size;
  entry.sha256 = session.sha.hexDigest();
  entry.created = util::nowMs();

  std::lock_guard<std::mutex> lock(mutex_);
  files_.push_back(entry);
  return true;
}

void FileStore::abortUpload(UploadSession& session) {
  if (session.out.is_open()) session.out.close();
  std::error_code ec;
  fs::remove(session.tmpPath, ec);
}

std::optional<FileEntry> FileStore::getFile(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& file : files_) {
    if (file.id == id) return file;
  }
  return std::nullopt;
}

std::vector<FileEntry> FileStore::filesOwnedBy(const std::string& owner) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FileEntry> out;
  for (const auto& file : files_) {
    if (file.owner == owner) out.push_back(file);
  }
  return out;
}

std::vector<FileEntry> FileStore::receivedFiles() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FileEntry> out;
  for (const auto& file : files_) {
    if (file.owner != "server") out.push_back(file);
  }
  std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.created > b.created; });
  return out;
}

bool FileStore::removeFile(const std::string& id) {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(files_.begin(), files_.end(), [&](const FileEntry& file) { return file.id == id; });
    if (it == files_.end()) return false;
    path = it->path;
    files_.erase(it);
    tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(),
                                 [&](const ShareToken& token) { return token.fileId == id; }),
                  tokens_.end());
  }
  std::error_code ec;
  if (!path.empty()) fs::remove(path, ec);
  return true;
}

std::string FileStore::issueToken(const std::string& fileId, const std::string& deviceId,
                                  const std::string& transferId, const std::string& batchId) {
  ShareToken token;
  token.token = util::randomHex(16);
  token.fileId = fileId;
  token.deviceId = deviceId;
  token.transferId = transferId;
  token.batchId = batchId;
  token.expires = util::nowMs() + kTokenTtlMs;

  std::lock_guard<std::mutex> lock(mutex_);
  tokens_.push_back(token);
  return token.token;
}

std::optional<ShareToken> FileStore::getToken(const std::string& value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& token : tokens_) {
    if (token.token == value) {
      if (token.expires < util::nowMs()) return std::nullopt;
      return token;
    }
  }
  return std::nullopt;
}

std::vector<ShareToken> FileStore::tokensForDevice(const std::string& deviceId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ShareToken> out;
  int64_t now = util::nowMs();
  for (const auto& token : tokens_) {
    if (token.deviceId == deviceId && token.expires >= now) out.push_back(token);
  }
  return out;
}

void FileStore::dropTokensForFile(const std::string& fileId) {
  std::lock_guard<std::mutex> lock(mutex_);
  tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(),
                               [&](const ShareToken& token) { return token.fileId == fileId; }),
                tokens_.end());
}

std::string FileStore::addTransfer(TransferRecord record) {
  if (record.id.empty()) record.id = util::randomHex(12);
  if (record.created == 0) record.created = util::nowMs();
  std::lock_guard<std::mutex> lock(mutex_);
  std::string id = record.id;
  transfers_.push_back(std::move(record));
  if (transfers_.size() > kMaxHistory) {
    transfers_.erase(transfers_.begin(), transfers_.begin() + static_cast<std::ptrdiff_t>(transfers_.size() - kMaxHistory));
  }
  return id;
}

bool FileStore::setTransferStatus(const std::string& id, const std::string& status) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& transfer : transfers_) {
    if (transfer.id == id) {
      transfer.status = status;
      return true;
    }
  }
  return false;
}

bool FileStore::setTransferBytes(const std::string& id, uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& transfer : transfers_) {
    if (transfer.id == id) {
      transfer.bytes = bytes;
      return true;
    }
  }
  return false;
}

bool FileStore::setTransferDigest(const std::string& id, const std::string& sha256) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& transfer : transfers_) {
    if (transfer.id == id) {
      transfer.sha256 = sha256;
      return true;
    }
  }
  return false;
}

std::optional<TransferRecord> FileStore::getTransfer(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& transfer : transfers_) {
    if (transfer.id == id) return transfer;
  }
  return std::nullopt;
}

std::vector<TransferRecord> FileStore::transfers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return transfers_;
}

std::vector<TransferRecord> FileStore::transfersForDevice(const std::string& deviceId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TransferRecord> out;
  for (const auto& transfer : transfers_) {
    if (transfer.deviceId == deviceId) out.push_back(transfer);
  }
  return out;
}

void FileStore::clearHistory() {
  std::lock_guard<std::mutex> lock(mutex_);
  transfers_.clear();
}
