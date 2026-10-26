#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sha256.h"

struct FileEntry {
  std::string id;
  std::string name;
  std::string relPath;
  std::string owner;
  std::string ownerName;
  std::string batchId;
  std::string path;
  std::string sha256;
  uint64_t size = 0;
  int64_t created = 0;
};

struct ShareToken {
  std::string token;
  std::string fileId;
  std::string deviceId;
  std::string transferId;
  std::string batchId;
  int64_t expires = 0;
};

struct TransferRecord {
  std::string id;
  std::string direction;  // "to-device" | "from-device"
  std::string deviceId;
  std::string deviceName;
  std::string fileId;
  std::string fileName;
  std::string sha256;
  std::string status;
  uint64_t size = 0;
  uint64_t bytes = 0;
  int64_t created = 0;
};

class UploadSession {
 public:
  std::string id;
  std::string name;
  std::string relPath;
  std::string owner;
  std::string ownerName;
  std::string batchId;
  std::string tmpPath;
  std::ofstream out;
  Sha256 sha;
  uint64_t size = 0;

  bool append(const char* data, size_t len);
  bool finish();
};

class FileStore {
 public:
  bool init(const std::string& directory, bool keepExisting, std::string& err);

  std::unique_ptr<UploadSession> beginUpload(const std::string& owner, const std::string& ownerName,
                                             const std::string& batchId, const std::string& relPath,
                                             const std::string& name, std::string& err);
  bool finishUpload(UploadSession& session, FileEntry& entry, std::string& err);
  void abortUpload(UploadSession& session);

  std::optional<FileEntry> getFile(const std::string& id) const;
  std::vector<FileEntry> filesOwnedBy(const std::string& owner) const;
  std::vector<FileEntry> receivedFiles() const;
  bool removeFile(const std::string& id);

  std::string issueToken(const std::string& fileId, const std::string& deviceId, const std::string& transferId,
                         const std::string& batchId);
  std::optional<ShareToken> getToken(const std::string& token) const;
  std::vector<ShareToken> tokensForDevice(const std::string& deviceId) const;
  void dropTokensForFile(const std::string& fileId);

  std::string addTransfer(TransferRecord record);
  bool setTransferStatus(const std::string& id, const std::string& status);
  bool setTransferBytes(const std::string& id, uint64_t bytes);
  bool setTransferDigest(const std::string& id, const std::string& sha256);
  std::optional<TransferRecord> getTransfer(const std::string& id) const;
  std::vector<TransferRecord> transfers() const;
  std::vector<TransferRecord> transfersForDevice(const std::string& deviceId) const;
  void clearHistory();

  std::string directory() const { return directory_; }

 private:
  mutable std::mutex mutex_;
  std::string directory_;
  std::string filesDir_;
  std::string tmpDir_;
  std::vector<FileEntry> files_;
  std::vector<ShareToken> tokens_;
  std::vector<TransferRecord> transfers_;
};
