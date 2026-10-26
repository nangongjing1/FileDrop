#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t len);
  std::string hexDigest();

 private:
  void processBlock(const uint8_t* block);

  uint32_t state_[8];
  uint64_t total_ = 0;
  uint8_t buffer_[64];
  size_t bufferLen_ = 0;
  bool finalized_ = false;
};
