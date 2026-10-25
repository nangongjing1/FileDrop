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

#include "http_server.h"

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
#include <fcntl.h>
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
struct AdapterEntry {
  unsigned long index = 0;
  NetInterface info;
};

std::vector<AdapterEntry> enumerateAdapters() {
  std::vector<AdapterEntry> result;
  ULONG size = 0;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return result;

  std::vector<char> buffer(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR) return result;

  for (auto* a = addrs; a != nullptr; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    AdapterEntry entry;
    entry.index = a->IfIndex;
    entry.info.name = utf8FromWide(a->FriendlyName ? a->FriendlyName : (a->Description ? a->Description : L""));
    entry.info.hasGateway = a->FirstGatewayAddress != nullptr;
    for (auto* gateway = a->FirstGatewayAddress; gateway != nullptr; gateway = gateway->Next) {
      if (!gateway->Address.lpSockaddr || gateway->Address.lpSockaddr->sa_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN] = {0};
      auto* sin = reinterpret_cast<sockaddr_in*>(gateway->Address.lpSockaddr);
      if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) {
        entry.info.gateway = text;
        break;
      }
    }
    for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
      if (!ua->Address.lpSockaddr) continue;
      if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN] = {0};
      auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
      if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) continue;
      entry.info.ip = text;
      int prefix = static_cast<int>(ua->OnLinkPrefixLength);
      if (prefix <= 0 || prefix > 32) prefix = 24;
      entry.info.prefix = prefix;
      entry.info.netmask = prefixToNetmask(prefix);
      break;
    }
    if (entry.info.ip.empty()) continue;
    result.push_back(entry);
  }
  return result;
}
#else
struct AdapterEntry {
  unsigned long index = 0;
  NetInterface info;
};

std::vector<AdapterEntry> enumerateAdapters() {
  std::vector<AdapterEntry> result;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return result;
  for (auto* it = list; it != nullptr; it = it->ifa_next) {
    if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
    if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
    char text[INET_ADDRSTRLEN] = {0};
    auto* sin = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
    if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) continue;

    int prefix = 24;
    if (it->ifa_netmask && it->ifa_netmask->sa_family == AF_INET) {
      auto* mask = reinterpret_cast<sockaddr_in*>(it->ifa_netmask);
      uint32_t bits = ntohl(mask->sin_addr.s_addr);
      int count = 0;
      while (count < 32 && (bits & (0x80000000u >> count))) ++count;
      prefix = count;
    }

    AdapterEntry entry;
    entry.info.ip = text;
    entry.info.name = it->ifa_name ? it->ifa_name : "";
    entry.info.prefix = prefix;
    entry.info.netmask = prefixToNetmask(prefix);
    entry.info.hasGateway = true;
    result.push_back(entry);
  }
  freeifaddrs(list);
  return result;
}
#endif

bool isLinkLocal(const std::string& ip) {
  return ip.rfind("169.254.", 0) == 0 || ip.rfind("127.", 0) == 0;
}

}  // namespace

std::string prefixToNetmask(int prefix) {
  if (prefix < 0) prefix = 0;
  if (prefix > 32) prefix = 32;
  uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
  char text[INET_ADDRSTRLEN] = {0};
  uint32_t networkOrder = htonl(mask);
  if (!inet_ntop(AF_INET, &networkOrder, text, sizeof text)) return "0.0.0.0";
  return text;
}

bool parseIPv4(const std::string& text, uint32_t& hostOrder) {
  in_addr addr{};
  if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
  hostOrder = ntohl(addr.s_addr);
  return true;
}

bool isPrivateIPv4(const std::string& ip) {
  uint32_t value = 0;
  if (!parseIPv4(ip, value)) return false;
  uint32_t a = (value >> 24) & 0xFF;
  uint32_t b = (value >> 16) & 0xFF;
  if (a == 10) return true;
  if (a == 172 && b >= 16 && b <= 31) return true;
  if (a == 192 && b == 168) return true;
  return false;
}

std::vector<NetInterface> localInterfaces() {
  std::vector<NetInterface> out;
  for (const auto& entry : enumerateAdapters()) {
    if (isLinkLocal(entry.info.ip)) continue;
    out.push_back(entry.info);
  }
  return out;
}

std::string localIPv4() {
  auto entries = enumerateAdapters();
  std::vector<NetInterface> usable;
  for (const auto& entry : entries) {
    if (!isLinkLocal(entry.info.ip)) usable.push_back(entry.info);
  }
  if (usable.empty()) {
    if (!entries.empty()) return entries.front().info.ip;
    return "127.0.0.1";
  }

#ifdef _WIN32
  DWORD bestIndex = 0;
  IPAddr target = inet_addr("8.8.8.8");
  if (GetBestInterface(target, &bestIndex) == NO_ERROR && bestIndex != 0) {
    for (const auto& entry : entries) {
      if (entry.index == bestIndex && !isLinkLocal(entry.info.ip)) return entry.info.ip;
    }
  }
#endif

  for (const auto& info : usable) {
    if (info.hasGateway) return info.ip;
  }
  return usable.front().ip;
}

std::string primaryLocalIp() { return localIPv4(); }

std::string selectLocalIpFor(const std::string& peerIp) {
  uint32_t peer = 0;
  auto interfaces = localInterfaces();
  if (interfaces.empty()) return localIPv4();
  if (!parseIPv4(peerIp, peer)) return localIPv4();

  for (const auto& info : interfaces) {
    uint32_t local = 0;
    if (!parseIPv4(info.ip, local)) continue;
    if (!info.hasGateway) continue;
    uint32_t mask = info.prefix >= 32 ? 0xFFFFFFFFu : (info.prefix <= 0 ? 0 : (0xFFFFFFFFu << (32 - info.prefix)));
    if ((local & mask) == (peer & mask)) return info.ip;
  }
  for (const auto& info : interfaces) {
    uint32_t local = 0;
    if (!parseIPv4(info.ip, local)) continue;
    uint32_t mask = info.prefix >= 32 ? 0xFFFFFFFFu : (info.prefix <= 0 ? 0 : (0xFFFFFFFFu << (32 - info.prefix)));
    if ((local & mask) == (peer & mask)) return info.ip;
  }

  size_t bestBits = 0;
  std::string best;
  for (const auto& info : interfaces) {
    uint32_t local = 0;
    if (!parseIPv4(info.ip, local)) continue;
    uint32_t diff = local ^ peer;
    size_t bits = 0;
    while (bits < 32 && (diff & (0x80000000u >> bits)) == 0) ++bits;
    if (bits > bestBits) {
      bestBits = bits;
      best = info.ip;
    }
  }
  if (bestBits >= 8) return best;
  return localIPv4();
}

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

std::string normalizeRelPath(const std::string& raw) {
  std::string normalized = raw;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  std::istringstream stream(normalized);
  std::string part;
  std::string out;
  while (std::getline(stream, part, '/')) {
    if (part.empty() || part == "." || part == "..") continue;
    part = sanitizeName(part);
    if (part.empty()) continue;
    if (!out.empty()) out += "/";
    out += part;
    if (out.size() > 300) break;
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

std::string defaultGateway() {
  for (const auto& info : localInterfaces()) {
    if (!info.gateway.empty() && !isLinkLocal(info.gateway)) return info.gateway;
  }
  return "";
}

std::string systemDnsServer() {
  auto servers = systemDnsServers();
  return servers.empty() ? std::string() : servers.front();
}

std::vector<std::string> systemDnsServers() {
  std::vector<std::string> servers;
#ifdef _WIN32
  ULONG size = 0;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_UNICAST;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return servers;
  std::vector<char> buffer(size);
  auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addrs, &size) != NO_ERROR) return servers;
  for (auto* a = addrs; a != nullptr; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    for (auto* dns = a->FirstDnsServerAddress; dns != nullptr; dns = dns->Next) {
      if (!dns->Address.lpSockaddr || dns->Address.lpSockaddr->sa_family != AF_INET) continue;
      char text[INET_ADDRSTRLEN] = {0};
      auto* sin = reinterpret_cast<sockaddr_in*>(dns->Address.lpSockaddr);
      if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text) || isLinkLocal(text)) continue;
      bool duplicate = false;
      for (const auto& existing : servers) {
        if (existing == text) duplicate = true;
      }
      if (!duplicate) servers.push_back(text);
    }
  }
#else
  std::ifstream in("/etc/resolv.conf");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string key, value;
    iss >> key >> value;
    if (key == "nameserver" && !value.empty()) servers.push_back(value);
  }
#endif
  return servers;
}

bool probeDnsServer(const std::string& ip, int timeoutMs) {
  uint32_t target = 0;
  if (!parseIPv4(ip, target)) return false;

  socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalidSocket) return false;
  net::setRecvTimeout(s, timeoutMs);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(target);

  const uint8_t query[] = {0x46, 0x44, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',
                           0x00, 0x00, 0x01, 0x00, 0x01};
  bool ok = false;
  if (sendto(s, reinterpret_cast<const char*>(query), sizeof query, 0, reinterpret_cast<sockaddr*>(&addr),
             sizeof addr) > 0) {
    char buffer[512];
    int received = recv(s, buffer, sizeof buffer, 0);
    ok = received >= 12 && (static_cast<uint8_t>(buffer[0]) == 0x46);
  }
  net::closeSocket(s);
  return ok;
}

std::string wifiSsid() {
#ifdef _WIN32
  int exitCode = 0;
  std::string output = runProcess("netsh.exe", "wlan show interfaces", false, exitCode);
  size_t pos = 0;
  while (pos < output.size()) {
    size_t end = output.find('\n', pos);
    if (end == std::string::npos) end = output.size();
    std::string line = output.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = trim(line.substr(0, colon));
    if (key == "SSID") {
      std::string value = trim(line.substr(colon + 1));
      if (!value.empty()) return value;
    }
  }
  return "";
#else
  return "";
#endif
}

std::string hostsFilePath() {
#ifdef _WIN32
  return "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
  return "/etc/hosts";
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

bool probeHttp(const std::string& ip, int port, int timeoutMs) {
  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocket) return false;

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &target.sin_addr) != 1) {
    net::closeSocket(s);
    return false;
  }

#ifdef _WIN32
  u_long nonBlocking = 1;
  ioctlsocket(s, FIONBIO, &nonBlocking);
#else
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

  bool ok = false;
  int connected = connect(s, reinterpret_cast<sockaddr*>(&target), sizeof target);
  if (connected == 0) {
    ok = true;
  } else {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(s, &writeSet);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int ready = select(static_cast<int>(s) + 1, nullptr, &writeSet, nullptr, &tv);
    if (ready > 0) {
      int error = 0;
#ifdef _WIN32
      int len = sizeof error;
#else
      socklen_t len = sizeof error;
#endif
      getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
      ok = error == 0;
    }
  }

  if (ok) {
#ifdef _WIN32
    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
#else
    flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
    const char request[] = "GET /health HTTP/1.1\r\nHost: health\r\nConnection: close\r\n\r\n";
    net::setSendTimeout(s, timeoutMs);
    net::setRecvTimeout(s, timeoutMs);
    if (net::sendAll(s, request, sizeof request - 1)) {
      char buffer[128] = {0};
      int received = recv(s, buffer, sizeof buffer - 1, 0);
      if (received <= 0 || std::string(buffer).find(" 200 ") == std::string::npos) ok = false;
    } else {
      ok = false;
    }
  }

  net::closeSocket(s);
  return ok;
}

std::string runProcess(const std::string& file, const std::string& args, bool elevate, int& exitCode) {
  exitCode = -1;
#ifdef _WIN32
  std::wstring wideFile = wideFromUtf8(file);
  std::wstring wideArgs = wideFromUtf8(args);

  // CreateProcessW 不会在 PATH 中查找程序名，需要先解析成完整路径。
  if (wideFile.find(L'\\') == std::wstring::npos && wideFile.find(L'/') == std::wstring::npos) {
    wchar_t resolved[MAX_PATH * 2] = {0};
    DWORD length = SearchPathW(nullptr, wideFile.c_str(), L".exe", MAX_PATH * 2, resolved, nullptr);
    if (length > 0 && length < MAX_PATH * 2) wideFile = resolved;
  }

  // 提权运行无法继承管道，只等待退出码。
  if (elevate) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = wideFile.c_str();
    info.lpParameters = wideArgs.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
      exitCode = GetLastError() == ERROR_CANCELLED ? -3 : -2;
      return "";
    }
    if (info.hProcess) {
      WaitForSingleObject(info.hProcess, 120000);
      DWORD code = 0;
      if (GetExitCodeProcess(info.hProcess, &code)) exitCode = static_cast<int>(code);
      CloseHandle(info.hProcess);
    }
    return "";
  }

  SECURITY_ATTRIBUTES attrs{};
  attrs.nLength = sizeof attrs;
  attrs.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &attrs, 0)) return "";
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdOutput = writePipe;
  startup.hStdError = writePipe;

  PROCESS_INFORMATION process{};
  std::wstring commandLine = L"\"" + wideFile + L"\" " + wideArgs;
  std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
  commandBuffer.push_back(L'\0');

  BOOL started = CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &process);
  CloseHandle(writePipe);
  if (!started) {
    CloseHandle(readPipe);
    exitCode = -1;
    return "";
  }

  std::string output;
  char buffer[1024];
  DWORD read = 0;
  while (output.size() < 65536 && ReadFile(readPipe, buffer, sizeof buffer, &read, nullptr) && read > 0) {
    output.append(buffer, read);
  }
  CloseHandle(readPipe);

  WaitForSingleObject(process.hProcess, 30000);
  DWORD code = 0;
  if (GetExitCodeProcess(process.hProcess, &code)) exitCode = static_cast<int>(code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return output;
#else
  if (elevate) return "";
  std::string command = file + " " + args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    exitCode = -2;
    return "";
  }
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  int status = pclose(pipe);
  exitCode = status;
  return output;
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
