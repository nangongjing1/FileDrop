#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "applog.h"
#include "dns_server.h"
#include "firewall.h"
#include "http_server.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_waitMutex;
std::condition_variable g_waitCv;

void handleSignal(int) {
  g_stop = true;
  g_waitCv.notify_all();
}

void printHelp(const char* name) {
  std::cout << "用法: " << name << " [选项]\n"
            << "  --port <端口>        HTTP 端口，默认 80(被占用时自动尝试 8080)\n"
            << "  --bind <地址>        监听地址，默认 0.0.0.0\n"
            << "  --data <目录>        数据目录，默认 filedrop-data\n"
            << "  --hostname <域名>    客户端访问域名，默认 filedrop.ng\n"
            << "  --alias <域名>       额外可解析的域名(可重复)\n"
            << "  --ip <地址>          对客户端公布的本机 IP(多网卡时手动指定)\n"
            << "  --no-open            启动时不自动打开浏览器\n"
            << "  --no-dns             不启动内置 DNS 服务\n"
            << "  --no-firewall        等同于 --firewall off\n"
            << "  --firewall <模式>    auto(自动添加规则，默认)| check(只检查)| off\n"
            << "  --dns-port <端口>    DNS 监听端口，默认 53\n"
            << "  --upstream <IP>      上游 DNS，默认读取系统配置\n"
            << "  --keep               保留数据目录中的旧文件\n"
            << "  --diagnose           只做环境体检并输出报告(不启动服务)\n"
            << "  --help               显示帮助\n";
}

std::string portSuffix(int port) { return port == 80 ? "" : (":" + std::to_string(port)); }

bool canBindTcp(int port) {
  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocket) return false;
  int reuse = 1;
#ifdef _WIN32
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  bool ok = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  net::closeSocket(s);
  return ok;
}

bool canBindUdp(int port) {
  socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kInvalidSocket) return false;
  int reuse = 1;
#ifdef _WIN32
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  bool ok = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  net::closeSocket(s);
  return ok;
}

std::string firewallPolicyLine() {
  firewall::Status status = firewall::inspect(80, 53, true);
  std::string text = status.profile.empty() ? std::string("未知") : status.profile;
  text += status.firewallEnabled ? "(防火墙已启用)" : "(防火墙已关闭)";
  if (status.blockAllInbound) text += "  ⚠ 阻止所有入站连接";
  return text;
}

std::string hostsEntryFor(const std::string& hostname) {
  std::ifstream in(util::hostsFilePath());
  std::string line;
  while (std::getline(in, line)) {
    std::string trimmed = util::trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    if (trimmed.find(hostname) != std::string::npos) return trimmed;
  }
  return "";
}

std::string selectUpstream(const std::string& forced, const std::string& selfIp) {
  if (!forced.empty()) return forced == selfIp ? std::string() : forced;
  std::vector<std::string> candidates = util::systemDnsServers();
  std::string gateway = util::defaultGateway();
  bool hasGateway = false;
  for (const auto& candidate : candidates) {
    if (candidate == gateway) hasGateway = true;
  }
  if (!gateway.empty() && !hasGateway) candidates.push_back(gateway);

  for (const auto& candidate : candidates) {
    if (candidate == selfIp) continue;
    if (util::probeDnsServer(candidate, 600)) return candidate;
  }
  for (const auto& candidate : candidates) {
    if (candidate != selfIp) return candidate;
  }
  return "";
}

int runDiagnose(const AppOptions& options, bool dnsEnabled, int dnsPort, const std::string& forcedIp) {
  std::string ip = forcedIp.empty() ? util::localIPv4() : forcedIp;
  std::string ssid = util::wifiSsid();
  std::string suffix = portSuffix(options.port);
  auto interfaces = util::localInterfaces();
  auto httpRule = firewall::inspect(options.port, dnsPort, dnsEnabled);

  std::ostringstream report;
  report << "Filedrop 诊断报告\n";
  report << "===============\n\n";
  report << "生成时间      : " << util::formatClock(util::nowMs()) << "\n";
  report << "客户端域名    : http://" << options.hostname << suffix << "/\n";
  report << "推荐的 IP 地址: http://" << ip << suffix << "/\n";
  report << "数据目录      : " << options.dataDir << "\n";
  report << "当前 Wi-Fi    : " << (ssid.empty() ? "(未连接到 Wi-Fi 或无线服务不可用)" : ssid) << "\n\n";

  report << "网卡地址\n--------\n";
  for (const auto& info : interfaces) {
    report << "  " << info.ip << "/" << info.netmask << "  " << info.name
           << (info.ip == ip ? "   <-- 当前使用" : "") << "\n";
  }
  if (interfaces.empty()) report << "  (没有找到可用的 IPv4 网卡)\n";
  report << "\n";

  report << "端口占用\n--------\n";
  report << "  TCP " << options.port << " : " << (canBindTcp(options.port) ? "可绑定" : "已被占用") << "\n";
  if (dnsEnabled) {
    report << "  UDP " << dnsPort << " : " << (canBindUdp(dnsPort) ? "可绑定" : "已被占用") << "\n";
  }
  report << "\n";

  report << "防火墙\n------\n";
  report << "  当前配置      : " << firewallPolicyLine() << "\n";
  report << "  规则检查      : " << httpRule.detail << "\n";
  if (httpRule.supported) {
    report << "  手动放行命令  :\n"
           << "    netsh advfirewall firewall add rule name=\"Filedrop HTTP " << options.port
           << "\" dir=in action=allow protocol=TCP localport=" << options.port << "\n";
    if (dnsEnabled) {
      report << "    netsh advfirewall firewall add rule name=\"Filedrop DNS " << dnsPort
             << "\" dir=in action=allow protocol=UDP localport=" << dnsPort << "\n";
    }
  }
  report << "\n";

  report << "DNS 与 hosts\n------------\n";
  report << "  系统 DNS      : " << (util::systemDnsServer().empty() ? "(未读取到)" : util::systemDnsServer()) << "\n";
  std::string effectiveUpstream = selectUpstream("", ip);
  report << "  转发用上游    : " << (effectiveUpstream.empty() ? "(无，外部域名将无法解析)" : effectiveUpstream) << "\n";
  std::string entry = hostsEntryFor(options.hostname);
  report << "  hosts 记录    : " << (entry.empty() ? "(未配置)" : entry) << "\n\n";

  report << "排查建议\n--------\n";
  report << "  1. 先在客户端浏览器访问 http://" << ip << suffix << "/ ，确认 IP 直连是否可用。\n";
  report << "  2. 手机与电脑必须在同一个 Wi-Fi(当前：" << (ssid.empty() ? "未知" : ssid)
         << ")，访客 Wi-Fi 通常彼此隔离。\n";
  report << "  3. 用域名访问前，需要把客户端 DNS 改成 " << ip << "，或在 hosts 中加入: " << ip << " "
         << options.hostname << "\n";
  report << "  4. 若 IP 直连也失败，请检查路由器是否开启 AP 隔离 / 客户端局域网访问限制。\n";

  std::string text = report.str();
  std::cout << text << std::flush;

  std::error_code ec;
  std::filesystem::create_directories(options.dataDir, ec);
  std::ofstream out(options.dataDir + "/diagnose.txt", std::ios::binary | std::ios::trunc);
  if (out) {
    out << "\xEF\xBB\xBF" << text;
    std::cout << "\n报告已保存到 " << options.dataDir << "/diagnose.txt\n";
  }
  return 0;
}

void writeHostsHelper(const AppOptions& options, const std::string& ip, int port) {
  std::string suffix = portSuffix(port);
  std::string names = options.hostname;
  for (const auto& alias : options.aliases) names += " " + alias;

  {
    std::ofstream out(options.dataDir + "/hosts-help.txt", std::ios::binary | std::ios::trunc);
    if (out) {
      out << "\xEF\xBB\xBF"  // BOM，保证记事本等工具能正确识别 UTF-8
          << "Filedrop 局域网访问助手\n"
          << "======================\n\n"
          << "客户端地址: http://" << options.hostname << suffix << "/\n"
          << "IP 地址   : http://" << ip << suffix << "/\n\n"
          << "方式一(推荐)：把手机/电脑的 DNS 改为 " << ip << "，然后访问 http://" << options.hostname
          << suffix << "/\n"
          << "方式二：在设备的 hosts 文件里加入下面一行，然后访问同样的地址\n\n"
          << "    " << ip << " " << names << "\n\n"
          << "Windows hosts: C:\\Windows\\System32\\drivers\\etc\\hosts\n"
          << "macOS / Linux: /etc/hosts\n"
          << "Android / iOS 不能修改 hosts，请使用方式一，或直接访问 IP 地址。\n";
    }
  }

  {
    std::ofstream out(options.dataDir + "/clients-hosts.ps1", std::ios::binary | std::ios::trunc);
    if (out) {
      out << "\xEF\xBB\xBF"  // BOM，避免中文注释在旧版 PowerShell 中乱码
          << "# 以管理员身份运行，把 " << options.hostname << " 指向本机\n"
          << "$hosts = \"$env:WINDIR\\System32\\drivers\\etc\\hosts\"\n"
          << "$entry = \"" << ip << " " << names << "\"\n"
          << "if (-not (Select-String -Path $hosts -Pattern '" << options.hostname << "' -Quiet)) {\n"
          << "  Add-Content -Path $hosts -Value $entry -Encoding ASCII\n"
          << "  Write-Host \"已写入: $entry\"\n"
          << "} else { Write-Host \"hosts 中已存在 " << options.hostname << " 记录\" }\n";
    }
  }
}

void printBanner(const AppOptions& options, const App& app, int port, const std::string& ip, bool dnsActive,
                 int dnsPort, const std::string& firewallText) {
  std::string suffix = portSuffix(port);
  std::cout << "\n============================================================\n"
            << "  Filedrop · 局域网文件传输\n"
            << "============================================================\n"
            << "  服务端界面    " << app.adminUrl() << "\n"
            << "  客户端地址    http://" << options.hostname << suffix << "/\n";
  for (const auto& alias : options.aliases) {
    std::cout << "  别名地址      http://" << alias << suffix << "/\n";
  }
  std::cout << "  本机地址      http://" << ip << suffix << "/   <-- 其它设备也可直接用这个\n"
            << "  数据目录      " << options.dataDir << "\n"
            << "------------------------------------------------------------\n"
            << "  DNS: " << (dnsActive ? ("已启动(UDP " + std::to_string(dnsPort) + ")")
                                    : std::string("未启用"))
            << "\n"
            << "  防火墙: " << firewallText << "\n";

  auto interfaces = util::localInterfaces();
  if (interfaces.size() > 1) {
    std::cout << "------------------------------------------------------------\n"
              << "  检测到多块网卡，请选择与客户端同一网段的地址：\n";
    for (const auto& info : interfaces) {
      std::cout << "    " << info.ip << "/" << info.netmask << "  " << info.name
                << (info.ip == ip ? "  (当前使用)" : "") << "\n";
    }
    std::cout << "  如选错，可用 --ip <地址> 指定。\n";
  }
  std::cout << "------------------------------------------------------------\n"
            << "  手机/平板请先连同一个 Wi-Fi，再访问上面的地址；\n"
            << "  用域名访问前，需把客户端 DNS 改成 " << ip << " 或修改 hosts。\n"
            << "------------------------------------------------------------\n"
            << "  命令: s 状态 | o 重新打开浏览器 | h 帮助 | q 退出\n"
            << "============================================================\n";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  AppOptions options;
  bool openBrowser = true;
  bool dnsEnabled = true;
  std::string firewallMode = "auto";
  int dnsPort = 53;
  std::string upstream;
  std::string forcedIp;
  bool portExplicit = false;
  bool diagnose = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "缺少参数: " << name << "\n";
        std::exit(1);
      }
      return argv[++i];
    };
    if (arg == "--port") {
      options.port = std::stoi(next("--port"));
      portExplicit = true;
    } else if (arg == "--bind") {
      options.bindAddress = next("--bind");
    } else if (arg == "--data") {
      options.dataDir = next("--data");
    } else if (arg == "--hostname") {
      options.hostname = next("--hostname");
    } else if (arg == "--alias") {
      options.aliases.push_back(next("--alias"));
    } else if (arg == "--ip") {
      forcedIp = next("--ip");
    } else if (arg == "--no-open") {
      openBrowser = false;
    } else if (arg == "--no-dns") {
      dnsEnabled = false;
    } else if (arg == "--no-firewall") {
      firewallMode = "off";
    } else if (arg == "--firewall") {
      firewallMode = util::toLower(next("--firewall"));
    } else if (arg == "--dns-port") {
      dnsPort = std::stoi(next("--dns-port"));
    } else if (arg == "--upstream") {
      upstream = next("--upstream");
    } else if (arg == "--keep") {
      options.keepData = true;
    } else if (arg == "--diagnose") {
      diagnose = true;
    } else if (arg == "--help" || arg == "-h") {
      printHelp(argv[0]);
      return 0;
    } else {
      std::cerr << "未知参数: " << arg << "\n";
      printHelp(argv[0]);
      return 1;
    }
  }

  // 保留旧域名作为别名，方便之前配置过 hosts 的设备继续访问。
  if (options.hostname != "filedrop.ng") {
    bool hasLegacy = false;
    for (const auto& alias : options.aliases) {
      if (alias == "filedrop.ng") hasLegacy = true;
    }
    if (!hasLegacy) options.aliases.push_back("filedrop.ng");
  }

  std::string err;
  if (!net::initSockets(err)) {
    std::cerr << "网络初始化失败: " << err << "\n";
    return 1;
  }

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif

  if (diagnose) {
    int code = runDiagnose(options, dnsEnabled, dnsPort, forcedIp);
    net::cleanupSockets();
    return code;
  }

  App app(options);
  if (!app.init(err)) {
    std::cerr << "启动失败: " << err << "\n";
    return 1;
  }
  applog::init(options.dataDir, !options.keepData);

  HttpServer server;
  app.registerRoutes(server);

  int port = options.port;
  if (!server.start(options.bindAddress, port, err)) {
    if (!portExplicit && port == 80) {
      std::cout << "端口 80 不可用(" << err << ")，尝试 8080 ...\n";
      port = 8080;
      if (!server.start(options.bindAddress, port, err)) {
        std::cerr << "启动失败: " << err << "\n";
        return 1;
      }
    } else {
      std::cerr << "启动失败: " << err << "\n";
      return 1;
    }
  }
  const int httpPort = server.port();

  std::string ip = forcedIp.empty() ? util::localIPv4() : forcedIp;
  uint32_t parsedIp = 0;
  if (!util::parseIPv4(ip, parsedIp)) {
    std::cerr << "本机 IP 地址无效: " << ip << "\n";
    return 1;
  }
  app.setRuntime(httpPort, ip);

  DnsServer dns;
  bool dnsActive = false;
  std::string dnsError;
  std::string upstreamUsed;
  if (dnsEnabled) {
    upstreamUsed = selectUpstream(upstream, ip);
    dns.setAnswerProvider([forcedIp](const std::string& clientIp) {
      return forcedIp.empty() ? util::selectLocalIpFor(clientIp) : forcedIp;
    });
    if (dns.start(options.hostname, options.aliases, parsedIp, upstreamUsed, static_cast<uint16_t>(dnsPort), err)) {
      dnsActive = true;
    } else {
      dnsError = err;
    }
  }
  app.setDnsActive(dnsActive);

  std::string initialFirewallText;
  if (firewallMode == "off") {
    initialFirewallText = "已跳过(--firewall off)";
  } else if (firewallMode == "check") {
    initialFirewallText = firewall::inspect(httpPort, dnsPort, dnsActive).detail;
  } else if (firewallMode == "auto") {
    initialFirewallText = "正在检查并尝试自动放行端口…";
  } else {
    initialFirewallText = "未知模式 " + firewallMode;
  }
  app.setFirewallReport(initialFirewallText);

  writeHostsHelper(options, ip, httpPort);
  app.setFirewallReport(initialFirewallText);
  printBanner(options, app, httpPort, ip, dnsActive, dnsPort, initialFirewallText);

  {
    std::ostringstream startup;
    startup << "启动 hostname=" << options.hostname << " http=" << httpPort << " ip=" << ip
            << " dns=" << (dnsActive ? std::to_string(dnsPort) : std::string("off"))
            << " upstream=" << (upstreamUsed.empty() ? "none" : upstreamUsed);
    for (const auto& info : util::localInterfaces()) startup << " iface=" << info.ip << "/" << info.netmask;
    std::string ssid = util::wifiSsid();
    if (!ssid.empty()) startup << " wifi=" << ssid;
    applog::info(startup.str());
    applog::info("防火墙: " + initialFirewallText);
    if (!dnsError.empty()) applog::info("DNS 启动失败: " + dnsError);
  }

  if (!dnsActive && dnsEnabled) {
    std::cout << "  [提示] 内置 DNS 未启动：" << dnsError
              << "\n         请改用 http://" << ip << portSuffix(httpPort)
              << "/ 访问，或在客户端修改 hosts(见数据目录 hosts-help.txt)。\n";
  }

  bool reachable = util::probeHttp(ip, httpPort, 2000);
  if (reachable) {
    std::cout << "  [自检] http://" << ip << portSuffix(httpPort) << "/ 本机回环访问正常。\n";
  } else {
    std::cout << "  [自检] 无法通过 " << ip << " 访问服务，请检查地址是否为本机网卡地址、"
              << "或安全软件是否拦截了监听端口。\n";
  }
  std::cout << std::flush;

  auto startupNoticesDone = std::make_shared<std::atomic<bool>>(firewallMode != "auto");

  if (firewallMode == "auto") {
    std::thread([&app, httpPort, dnsPort, dnsActive, startupNoticesDone] {
      firewall::Status status = firewall::ensureRules(httpPort, dnsPort, dnsActive);
      app.setFirewallReport(status.detail);
      std::cout << "  [防火墙] " << status.detail << "\n";
      if (!status.rulesPresent) {
        std::cout << "           以管理员身份执行以下命令可手动放行：\n"
                  << "             netsh advfirewall firewall add rule name=\"Filedrop HTTP " << httpPort
                  << "\" dir=in action=allow protocol=TCP localport=" << httpPort << "\n";
        if (dnsActive) {
          std::cout << "             netsh advfirewall firewall add rule name=\"Filedrop DNS " << dnsPort
                    << "\" dir=in action=allow protocol=UDP localport=" << dnsPort << "\n";
        }
      }
      std::cout << std::flush;
      startupNoticesDone->store(true);
    }).detach();
  }

  if (openBrowser) {
    if (!util::openBrowser(app.adminUrl())) {
      std::cout << "未能自动打开浏览器，请手动访问上面的服务端界面地址。\n";
    }
  }

  const int finalDnsPort = dnsPort;
  std::thread inputThread([&] {
    while (!startupNoticesDone->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::string line;
    while (true) {
      std::cout << ">> " << std::flush;
      if (!std::getline(std::cin, line)) break;
      std::string cmd = util::toLower(util::trim(line));
      if (cmd == "q" || cmd == "quit" || cmd == "exit") {
        g_stop = true;
        g_waitCv.notify_all();
        break;
      }
      if (cmd == "o" || cmd == "open") {
        util::openBrowser(app.adminUrl());
      } else if (cmd == "s" || cmd == "status") {
        std::cout << "监听端口 " << httpPort << "\n"
                  << "客户端地址 http://" << options.hostname << portSuffix(httpPort) << "/\n"
                  << "本机地址 http://" << ip << portSuffix(httpPort) << "/\n"
                  << "DNS " << (dnsActive ? "运行中" : "未运行") << " (UDP " << finalDnsPort << ")\n"
                  << "防火墙 " << app.firewallSnapshot() << "\n"
                  << "数据目录 " << options.dataDir << "\n"
                  << std::flush;
      } else if (cmd == "h" || cmd == "help" || cmd == "?") {
        std::cout << "命令: s 状态 | o 重新打开浏览器 | h 帮助 | q 退出\n" << std::flush;
      }
    }
  });
  inputThread.detach();

  {
    std::unique_lock<std::mutex> lock(g_waitMutex);
    while (!g_stop) {
      g_waitCv.wait_for(lock, std::chrono::milliseconds(300));
    }
  }

  app.stop();
  dns.stop();
  server.stop();
  net::cleanupSockets();
  std::cout << "\nFiledrop 已退出。\n";
  return 0;
}
