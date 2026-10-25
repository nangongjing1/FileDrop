#include "applog.h"

#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

#include "util.h"

namespace applog {
namespace {

std::mutex g_mutex;
std::ofstream g_stream;
std::string g_path;
std::deque<std::string> g_recent;
constexpr size_t kRecentLimit = 400;

}  // namespace

bool init(const std::string& directory, bool truncate) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_path = directory + "/filedrop.log";
  g_recent.clear();
  g_stream.close();
  g_stream.open(g_path, std::ios::binary | std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
  return static_cast<bool>(g_stream);
}

void info(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string line = "[" + util::formatClock(util::nowMs()) + "] " + message;
  g_recent.push_back(line);
  while (g_recent.size() > kRecentLimit) g_recent.pop_front();
  if (g_stream) {
    g_stream << line << "\n";
    g_stream.flush();
  }
}

std::string tail(size_t maxLines) {
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t start = g_recent.size() > maxLines ? g_recent.size() - maxLines : 0;
  std::ostringstream out;
  for (size_t i = start; i < g_recent.size(); ++i) {
    out << g_recent[i] << "\n";
  }
  return out.str();
}

std::string filePath() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_path;
}

}  // namespace applog
