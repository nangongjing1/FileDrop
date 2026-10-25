#ifdef _WIN32
// 让 MinGW 头文件里用 DEFINE_GUID 声明的 CLSID/IID 真正生成定义(否则链接不到)。
#define INITGUID
#endif

#include "firewall.h"

#include <string>
#include <vector>

#include "util.h"

#ifdef _WIN32
#include <filesystem>
#include <fstream>
#include <netfw.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <oleauto.h>
#endif

namespace firewall {
namespace {

std::string ruleName(const char* kind, int port) {
  return std::string("Filedrop ") + kind + " " + std::to_string(port);
}

#ifdef _WIN32

struct ComScope {
  HRESULT hr = E_FAIL;
  ComScope() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
  ~ComScope() {
    if (SUCCEEDED(hr)) CoUninitialize();
  }
};

struct VariantWrap {
  VARIANT value;
  VariantWrap() { VariantInit(&value); }
  ~VariantWrap() { VariantClear(&value); }
};

std::string bstrToUtf8(BSTR value) {
  if (!value) return "";
  int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return "";
  std::string out(static_cast<size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, &out[0], length, nullptr, nullptr);
  return out;
}

const char* profileName(NET_FW_PROFILE_TYPE2 profile) {
  switch (profile) {
    case NET_FW_PROFILE2_DOMAIN: return "域";
    case NET_FW_PROFILE2_PRIVATE: return "专用";
    case NET_FW_PROFILE2_PUBLIC: return "公用";
    default: return "未知";
  }
}

NET_FW_PROFILE_TYPE2 currentProfile(INetFwPolicy2* policy) {
  long types = 0;
  if (FAILED(policy->get_CurrentProfileTypes(&types))) return NET_FW_PROFILE2_PUBLIC;
  if (types & NET_FW_PROFILE2_DOMAIN) return NET_FW_PROFILE2_DOMAIN;
  if (types & NET_FW_PROFILE2_PRIVATE) return NET_FW_PROFILE2_PRIVATE;
  return NET_FW_PROFILE2_PUBLIC;
}

bool ruleExists(INetFwPolicy2* policy, const std::string& name) {
  INetFwRules* rules = nullptr;
  if (FAILED(policy->get_Rules(&rules)) || !rules) return false;

  IUnknown* unknown = nullptr;
  bool found = false;
  if (SUCCEEDED(rules->get__NewEnum(&unknown)) && unknown) {
    IEnumVARIANT* enumerator = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void**>(&enumerator)))) {
      for (;;) {
        VariantWrap item;
        ULONG fetched = 0;
        if (enumerator->Next(1, &item.value, &fetched) != S_OK || fetched == 0) break;
        if (item.value.vt != VT_DISPATCH || !item.value.pdispVal) continue;

        INetFwRule* rule = nullptr;
        if (FAILED(item.value.pdispVal->QueryInterface(IID_INetFwRule, reinterpret_cast<void**>(&rule))) || !rule) {
          continue;
        }
        BSTR ruleNameValue = nullptr;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (SUCCEEDED(rule->get_Name(&ruleNameValue)) && ruleNameValue) {
          std::string ruleNameText = bstrToUtf8(ruleNameValue);
          rule->get_Enabled(&enabled);
          if (ruleNameText == name && enabled == VARIANT_TRUE) found = true;
        }
        if (ruleNameValue) SysFreeString(ruleNameValue);
        rule->Release();
        if (found) break;
      }
      enumerator->Release();
    }
    unknown->Release();
  }
  rules->Release();
  return found;
}

#else

bool ruleExists(const std::string& name) {
  (void)name;
  return true;
}

#endif

}  // namespace

Status inspect(int httpPort, int dnsPort, bool includeDns) {
  Status status;
#ifdef _WIN32
  status.supported = true;
  ComScope com;
  INetFwPolicy2* policy = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_NetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, IID_INetFwPolicy2,
                                reinterpret_cast<void**>(&policy));
  if (FAILED(hr) || !policy) {
    status.detail = "无法读取防火墙状态(COM 初始化失败)";
    return status;
  }

  NET_FW_PROFILE_TYPE2 profile = currentProfile(policy);
  status.profile = profileName(profile);

  VARIANT_BOOL enabled = VARIANT_FALSE;
  VARIANT_BOOL blockAll = VARIANT_FALSE;
  policy->get_FirewallEnabled(profile, &enabled);
  policy->get_BlockAllInboundTraffic(profile, &blockAll);
  status.firewallEnabled = enabled == VARIANT_TRUE;
  status.blockAllInbound = blockAll == VARIANT_TRUE;

  bool httpOk = ruleExists(policy, ruleName("HTTP", httpPort));
  bool dnsOk = !includeDns || ruleExists(policy, ruleName("DNS", dnsPort));
  policy->Release();

  status.rulesPresent = httpOk && dnsOk;
  if (!status.firewallEnabled) {
    status.detail = "防火墙已关闭，入站不会被拦截";
  } else if (status.blockAllInbound) {
    status.detail = "防火墙策略为「阻止所有入站连接」，规则不会生效";
  } else if (status.rulesPresent) {
    status.detail = "入站规则已存在(" + status.profile + "配置文件)";
  } else {
    std::string missing = "缺少入站规则: ";
    bool first = true;
    if (!httpOk) {
      missing += "TCP " + std::to_string(httpPort);
      first = false;
    }
    if (!dnsOk) {
      if (!first) missing += "、";
      missing += "UDP " + std::to_string(dnsPort);
    }
    status.detail = missing;
  }
#else
  status.supported = false;
  status.rulesPresent = true;
  status.detail = "当前系统无需配置防火墙规则";
#endif
  return status;
}

Status ensureRules(int httpPort, int dnsPort, bool includeDns) {
  Status status = inspect(httpPort, dnsPort, includeDns);
  if (!status.supported || status.rulesPresent) return status;
  if (status.blockAllInbound) {
    status.detail = "防火墙策略为「阻止所有入站连接」，请在防火墙设置中允许入站";
    return status;
  }

#ifdef _WIN32
  std::vector<std::string> commands;
  {
    ComScope com;
    INetFwPolicy2* policy = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, IID_INetFwPolicy2,
                                   reinterpret_cast<void**>(&policy))) &&
        policy) {
      if (!ruleExists(policy, ruleName("HTTP", httpPort))) {
        commands.push_back("netsh advfirewall firewall add rule name=\"" + ruleName("HTTP", httpPort) +
                           "\" dir=in action=allow protocol=TCP localport=" + std::to_string(httpPort) +
                           " profile=any");
      }
      if (includeDns && !ruleExists(policy, ruleName("DNS", dnsPort))) {
        commands.push_back("netsh advfirewall firewall add rule name=\"" + ruleName("DNS", dnsPort) +
                           "\" dir=in action=allow protocol=UDP localport=" + std::to_string(dnsPort) +
                           " profile=any");
      }
      policy->Release();
    }
  }

  if (commands.empty()) {
    status.rulesPresent = true;
    status.detail = "入站规则已存在";
    return status;
  }

  std::filesystem::path scriptPath;
  {
    std::error_code ec;
    auto tempDir = std::filesystem::temp_directory_path(ec);
    if (ec) tempDir = std::filesystem::current_path(ec);
    scriptPath = tempDir / ("filedrop-firewall-" + util::randomHex(4) + ".cmd");
    std::ofstream script(scriptPath, std::ios::binary | std::ios::trunc);
    if (!script) {
      status.detail = "无法创建临时脚本，请手动放行端口";
      return status;
    }
    script << "@echo off\r\n";
    for (const auto& command : commands) script << command << "\r\n";
  }

  int exitCode = -1;
  util::runProcess("cmd.exe", "/c \"" + scriptPath.string() + "\"", true, exitCode);

  std::error_code ec;
  std::filesystem::remove(scriptPath, ec);

  if (exitCode == -2 || exitCode == -3) {
    status.blocked = true;
    status.detail = "已取消管理员授权，需手动放行端口";
    return status;
  }

  Status after = inspect(httpPort, dnsPort, includeDns);
  status.rulesPresent = after.rulesPresent;
  status.created = after.rulesPresent;
  status.detail = after.rulesPresent ? "已自动添加入站规则" : "自动添加规则失败，请手动放行端口";
  return status;
#else
  return status;
#endif
}

}  // namespace firewall
