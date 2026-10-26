#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace util {

int64_t nowMs();
std::string formatClock(int64_t ms);
std::string formatBytes(uint64_t bytes);
std::string randomHex(size_t bytes);
std::string urlDecode(const std::string& s);
std::string urlEncode(const std::string& s);
std::string sanitizeName(const std::string& name);
std::string normalizeRelPath(const std::string& raw);
std::string trim(const std::string& s);
std::string toLower(std::string s);

struct NetInterface {
  std::string ip;
  std::string netmask;
  std::string name;
  std::string gateway;
  int prefix = 24;
  bool hasGateway = false;
};

std::vector<NetInterface> localInterfaces();
std::string localIPv4();
std::string primaryLocalIp();
std::string selectLocalIpFor(const std::string& peerIp);
std::string prefixToNetmask(int prefix);
bool parseIPv4(const std::string& text, uint32_t& hostOrder);
bool isPrivateIPv4(const std::string& ip);

std::string systemDnsServer();
std::vector<std::string> systemDnsServers();
std::string defaultGateway();
bool probeDnsServer(const std::string& ip, int timeoutMs);
std::string wifiSsid();
std::string hostsFilePath();
bool openBrowser(const std::string& url);
bool probeHttp(const std::string& ip, int port, int timeoutMs);
std::string runProcess(const std::string& file, const std::string& args, bool elevate, int& exitCode);

std::string jsonEscape(const std::string& s);
std::string jsonQuote(const std::string& s);
std::string htmlEscape(const std::string& s);

struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object };

  Type type = Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  const JsonValue* find(const std::string& key) const;
  std::string stringOr(const std::string& key, const std::string& def = "") const;
  std::vector<std::string> stringArray(const std::string& key) const;
};

bool parseJson(const std::string& text, JsonValue& out, std::string& err);

}  // namespace util
