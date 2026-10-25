#include "util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace util {
namespace {

bool isUnreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

#ifdef _WIN32
std::string utf8FromWide(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring wideFromUtf8(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
  return out;
}
#endif

#ifdef _WIN32
struct AdapterInfo {
  DWORD index = 0;
  std::string ip;
  bool hasGateway = false;
  std::string description;
};

std::vector<AdapterInfo> enumerateAdapters() {
  std::vector<AdapterInfo> result;
  ULONG size = 0;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return result;

  std::vector<char> buffer(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR) return result;

  for (auto* a = addrs; a != nullptr; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    AdapterInfo info;
    info.index = a->IfIndex;
    info.description = utf8FromWide(a->Description ? a->Description : L"");
    info.hasGateway = a->FirstGatewayAddress != nullptr;
    for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
      if (!ua->Address.lpSockaddr) continue;
      if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN] = {0};
      auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
      if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) {
        info.ip = text;
        break;
      }
    }
    if (info.ip.empty()) continue;
    result.push_back(info);
  }
  return result;
}
#else
struct AdapterInfo {
  std::string ip;
  bool hasGateway = false;
  std::string description;
};

std::vector<AdapterInfo> enumerateAdapters() {
  std::vector<AdapterInfo> result;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (auto* it = list; it != nullptr; it = it->ifa_next) {
    if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
    if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
    char text[INET_ADDRSTRLEN] = {0};
    auto* sin = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
    if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) continue;
    AdapterInfo info;
    info.ip = text;
    info.description = it->ifa_name ? it->ifa_name : "";
    info.hasGateway = true;
    result.push_back(info);
  }
  freeifaddrs(list);
  return result;
}
#endif

bool isLinkLocal(const std::string& ip) {
  return ip.rfind("169.254.", 0) == 0 || ip.rfind("127.", 0) == 0;
}

}  // namespace

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string formatClock(int64_t ms) {
  std::time_t secs = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &secs);
#else
  localtime_r(&secs, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

std::string formatBytes(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  char buf[64];
  if (unit == 0) {
    std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buf, sizeof buf, "%.1f %s", value, units[unit]);
  }
  return buf;
}

std::string randomHex(size_t bytes) {
  static thread_local std::mt19937_64 rng([] {
    std::random_device rd;
    uint64_t seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    seed ^= static_cast<uint64_t>(nowMs()) * 0x9E3779B97F4A7C15ULL;
    seed ^= reinterpret_cast<uintptr_t>(&rd);
    return seed;
  }());
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    uint32_t v = static_cast<uint32_t>(rng() & 0xFF);
    out.push_back(digits[v >> 4]);
    out.push_back(digits[v & 0x0F]);
  }
  return out;
}

std::string urlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return ch - 'A' + 10;
      };
      out.push_back(static_cast<char>((hex(s[i + 1]) << 4) | hex(s[i + 2])));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string urlEncode(const std::string& s) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(digits[c >> 4]);
      out.push_back(digits[c & 0x0F]);
    }
  }
  return out;
}

std::string sanitizeName(const std::string& name) {
  std::string base = name;
  size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);

  std::string out;
  out.reserve(base.size());
  for (unsigned char c : base) {
    if (c < 0x20 || c == 0x7F) continue;
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.empty() || out == "." || out == "..") out = "unnamed";
  if (out.size() > 180) {
    size_t cut = 180;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t start = 0;
  size_t end = s.size();
  while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return s.substr(start, end - start);
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string localIPv4() {
  auto adapters = enumerateAdapters();
  if (adapters.empty()) return "127.0.0.1";

#ifdef _WIN32
  DWORD bestIndex = 0;
  IPAddr target = inet_addr("8.8.8.8");
  if (GetBestInterface(target, &bestIndex) == NO_ERROR && bestIndex != 0) {
    for (const auto& a : adapters) {
      if (a.index == bestIndex && !isLinkLocal(a.ip)) return a.ip;
    }
  }
#endif

  for (const auto& a : adapters) {
    if (!isLinkLocal(a.ip) && a.hasGateway) return a.ip;
  }
  for (const auto& a : adapters) {
    if (!isLinkLocal(a.ip)) return a.ip;
  }
  return adapters.front().ip;
}

std::string systemDnsServer() {
#ifdef _WIN32
  ULONG size = 0;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_UNICAST;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return "";
  std::vector<char> buffer(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR) return "";
  for (auto* a = addrs; a != nullptr; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    for (auto* dns = a->FirstDnsServerAddress; dns != nullptr; dns = dns->Next) {
      if (!dns->Address.lpSockaddr || dns->Address.lpSockaddr->sa_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN] = {0};
      auto* sin = reinterpret_cast<sockaddr_in*>(dns->Address.lpSockaddr);
      if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text) && !isLinkLocal(text)) return text;
    }
  }
  return "";
#else
  std::ifstream in("/etc/resolv.conf");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string key, value;
    iss >> key >> value;
    if (key == "nameserver" && !value.empty()) return value;
  }
  return "";
#endif
}

bool openBrowser(const std::string& url) {
#ifdef _WIN32
  std::wstring wide = wideFromUtf8(url);
  HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(result) > 32;
#else
  pid_t pid = fork();
  if (pid == 0) {
#if defined(__APPLE__)
    execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
    execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
    _exit(1);
  }
  return pid > 0;
#endif
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string jsonQuote(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string htmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

const JsonValue* JsonValue::find(const std::string& key) const {
  if (type != Object) return nullptr;
  for (const auto& kv : obj) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string JsonValue::stringOr(const std::string& key, const std::string& def) const {
  const JsonValue* v = find(key);
  if (!v || v->type != String) return def;
  return v->str;
}

std::vector<std::string> JsonValue::stringArray(const std::string& key) const {
  std::vector<std::string> out;
  const JsonValue* v = find(key);
  if (!v || v->type != Array) return out;
  for (const auto& item : v->arr) {
    if (item.type == String) out.push_back(item.str);
  }
  return out;
}

namespace {

struct JsonParser {
  const std::string& text;
  size_t pos = 0;
  std::string error;

  explicit JsonParser(const std::string& t) : text(t) {}

  void skipSpace() {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
      ++pos;
    }
  }

  bool fail(const std::string& message) {
    if (error.empty()) error = message + " @" + std::to_string(pos);
    return false;
  }

  bool parseString(std::string& out) {
    if (pos >= text.size() || text[pos] != '"') return fail("expected string");
    ++pos;
    out.clear();
    while (pos < text.size()) {
      unsigned char c = static_cast<unsigned char>(text[pos++]);
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (pos >= text.size()) return fail("bad escape");
      char esc = text[pos++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos + 4 > text.size()) return fail("bad unicode escape");
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = text[pos++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
            else return fail("bad unicode digit");
          }
          if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
            pos += 2;
            uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
              char h = text[pos++];
              low <<= 4;
              if (h >= '0' && h <= '9') low |= static_cast<uint32_t>(h - '0');
              else if (h >= 'a' && h <= 'f') low |= static_cast<uint32_t>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') low |= static_cast<uint32_t>(h - 'A' + 10);
              else return fail("bad unicode digit");
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          appendUtf8(out, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool parseValue(JsonValue& value) {
    skipSpace();
    if (pos >= text.size()) return fail("unexpected end");
    char c = text[pos];
    if (c == '"') {
      value.type = JsonValue::String;
      return parseString(value.str);
    }
    if (c == '{') {
      value.type = JsonValue::Object;
      ++pos;
      skipSpace();
      if (pos < text.size() && text[pos] == '}') {
        ++pos;
        return true;
      }
      while (true) {
        skipSpace();
        std::string key;
        if (!parseString(key)) return false;
        skipSpace();
        if (pos >= text.size() || text[pos] != ':') return fail("expected ':'");
        ++pos;
        JsonValue child;
        if (!parseValue(child)) return false;
        value.obj.emplace_back(std::move(key), std::move(child));
        skipSpace();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == '}') {
          ++pos;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      value.type = JsonValue::Array;
      ++pos;
      skipSpace();
      if (pos < text.size() && text[pos] == ']') {
        ++pos;
        return true;
      }
      while (true) {
        JsonValue child;
        if (!parseValue(child)) return false;
        value.arr.push_back(std::move(child));
        skipSpace();
        if (pos < text.size() && text[pos] == ',') {
          ++pos;
          continue;
        }
        if (pos < text.size() && text[pos] == ']') {
          ++pos;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == 't' && text.compare(pos, 4, "true") == 0) {
      pos += 4;
      value.type = JsonValue::Bool;
      value.boolean = true;
      return true;
    }
    if (c == 'f' && text.compare(pos, 5, "false") == 0) {
      pos += 5;
      value.type = JsonValue::Bool;
      value.boolean = false;
      return true;
    }
    if (c == 'n' && text.compare(pos, 4, "null") == 0) {
      pos += 4;
      value.type = JsonValue::Null;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = pos;
      if (text[pos] == '-') ++pos;
      while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
      if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
      }
      if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
      }
      value.type = JsonValue::Number;
      value.number = std::strtod(text.substr(start, pos - start).c_str(), nullptr);
      return true;
    }
    return fail("unexpected character");
  }
};

}  // namespace

bool parseJson(const std::string& text, JsonValue& out, std::string& err) {
  JsonParser parser(text);
  out = JsonValue();
  if (!parser.parseValue(out)) {
    err = parser.error;
    return false;
  }
  parser.skipSpace();
  if (parser.pos != text.size()) {
    err = "trailing characters";
    return false;
  }
  return true;
}

}  // namespace util
