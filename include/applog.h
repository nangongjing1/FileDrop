#pragma once

#include <cstddef>
#include <string>

namespace applog {

// 打开日志文件；truncate 为 true 时清空旧内容。日志写入失败不会影响主流程。
bool init(const std::string& directory, bool truncate);
void info(const std::string& message);
std::string tail(size_t maxLines);
std::string filePath();

}  // namespace applog
