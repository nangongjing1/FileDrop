#pragma once

#include <string>

namespace firewall {

struct Status {
  bool supported = false;
  bool firewallEnabled = false;
  bool blockAllInbound = false;
  bool rulesPresent = false;
  bool created = false;
  bool blocked = false;
  std::string profile;
  std::string detail;
};

// 只检查现有配置，不做修改。
Status inspect(int httpPort, int dnsPort, bool includeDns);

// 检查并在缺失时尝试以管理员权限添加规则(Windows 会弹出 UAC 确认)。
Status ensureRules(int httpPort, int dnsPort, bool includeDns);

}  // namespace firewall
