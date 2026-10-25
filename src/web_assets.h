#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

struct WebAsset {
  const char* mime;
  const char* data;
  size_t size;
};

const std::unordered_map<std::string, WebAsset>& webAssets();
