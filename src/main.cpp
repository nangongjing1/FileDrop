#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app.h"
#include "dns_server.h"
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
            << "  --port <端口>        HTTP 端口，默认 80（被占用时自动尝试 8080）\n"
            << "  --bind <地址>        监听地址，默认 0.0.0.0\n"
            << "  --data <目录>        数据目录，默认 filedrop-data\n"
            << "  --hostname <域名>    客户端访问域名，默认 filedrop.ng\n"
            << "  --no-open            启动时不自动打开浏览器\n"
            << "  --no-dns             不启动内置 DNS 服务\n"
            << "  --dns-port <端口>    DNS 监听端口，默认 53\n"
            << "  --upstream <IP>      上游 DNS，默认读取系统配置\n"
            << "  --keep               保留数据目录中的旧文件\n"
            << "  --help               显示帮助\n";
}

void writeHostsHelper(const App& app, const std::string& ip, const std::string& hostname) {
  std::string dir = app.dataDirectory();
  {
    std::ofstream out(dir + "/hosts-help.txt", std::ios::binary | std::ios::trunc);
    if (out) {
      out << "Filedrop 局域网访问助手\n"
          << "======================\n\n"
          << "方式一（推荐）：把手机/电脑的 DNS 改为 " << ip << "，然后访问 http://" << hostname << "/\n"
          << "方式二：在设备的 hosts 文件里加入下面一行，然后访问 http://" << hostname << "/\n\n"
          << "    " << ip << " " << hostname << "\n\n"
          << "Windows hosts: C:\\Windows\\System32\\drivers\\etc\\hosts\n"
          << "macOS / Linux: /etc/hosts\n"
          << "Android / iOS 无法修改 hosts，请使用方式一。\n";
    }
  }
  {
    std::ofstream out(dir + "/clients-hosts.ps1", std::ios::binary | std::ios::trunc);
    if (out) {
      out << "# 以管理员身份运行，将 filedrop.ng 指向本机（仅修改当前这台电脑）\n"
          << "$hosts = \"$env:WINDIR\\System32\\drivers\\etc\\hosts\"\n"
          << "$entry = \"" << ip << " " << hostname << "\"\n"
          << "if (-not (Select-String -Path $hosts -Pattern '" << hostname << "' -Quiet)) {\n"
          << "  Add-Content -Path $hosts -Value $entry -Encoding ASCII\n"
          << "  Write-Host \"已写入: $entry\"\n"
          << "} else { Write-Host \"hosts 中已存在 $hostname 记录\" }\n";
    }
  }
}

std::string portSuffix(int port) { return port == 80 ? "" : (":" + std::to_string(port)); }

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  AppOptions options;
  bool openBrowser = true;
  bool dnsEnabled = true;
  int dnsPort = 53;
  std::string upstream;
  bool portExplicit = false;

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
    } else if (arg == "--no-open") {
      openBrowser = false;
    } else if (arg == "--no-dns") {
      dnsEnabled = false;
    } else if (arg == "--dns-port") {
      dnsPort = std::stoi(next("--dns-port"));
    } else if (arg == "--upstream") {
      upstream = next("--upstream");
    } else if (arg == "--keep") {
      options.keepData = true;
    } else if (arg == "--help" || arg == "-h") {
      printHelp(argv[0]);
      return 0;
    } else {
      std::cerr << "未知参数: " << arg << "\n";
      printHelp(argv[0]);
      return 1;
    }
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

  App app(options);
  if (!app.init(err)) {
    std::cerr << "启动失败: " << err << "\n";
    return 1;
  }

  HttpServer server;
  app.registerRoutes(server);

  int port = options.port;
  if (!server.start(options.bindAddress, port, err)) {
    if (!portExplicit && port == 80) {
      std::cout << "端口 80 不可用，尝试 8080 ...\n";
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

  std::string ip = util::localIPv4();
  app.setRuntime(server.port(), ip);

  DnsServer dns;
  bool dnsActive = false;
  if (dnsEnabled) {
    uint32_t answerIp = 0;
    if (!DnsServer::parseIp(ip, answerIp)) {
      std::cout << "无法解析本机地址，跳过内置 DNS。\n";
    } else {
      std::string upstreamIp = upstream.empty() ? util::systemDnsServer() : upstream;
      if (upstreamIp == ip) upstreamIp.clear();
      if (dns.start(options.hostname, answerIp, upstreamIp, static_cast<uint16_t>(dnsPort), err)) {
        dnsActive = true;
      } else {
        std::cout << "内置 DNS 未启动: " << err << "\n"
                  << "  如端口 53 已被占用，可改用 hosts 方式访问（见数据目录中的 hosts-help.txt）。\n";
      }
    }
  }
  app.setDnsActive(dnsActive);
  writeHostsHelper(app, ip, options.hostname);

  std::string suffix = portSuffix(server.port());
  std::cout << "\n============================================================\n"
            << "  Filedrop · 局域网文件传输\n"
            << "============================================================\n"
            << "  服务端界面    " << app.adminUrl() << "\n"
            << "  局域网地址    http://" << ip << suffix << "/\n"
            << "  客户端地址    http://" << options.hostname << suffix << "/\n"
            << "  数据目录      " << options.dataDir << "\n"
            << "------------------------------------------------------------\n"
            << "  DNS: " << (dnsActive ? ("已在本机启动（端口 " + std::to_string(dnsPort) + "）")
                                      : std::string("未启用"))
            << "\n"
            << "  其它设备把 DNS 指向 " << ip << " 后即可访问 " << options.hostname << "\n"
            << "  无法修改 DNS 的设备，请参考 " << options.dataDir + "/hosts-help.txt\n"
            << "------------------------------------------------------------\n"
            << "  命令: s 状态 | o 重新打开浏览器 | h 帮助 | q 退出\n"
            << "============================================================\n";
#ifdef _WIN32
  std::cout << "  若其它设备无法访问，可用管理员 PowerShell 放行端口:\n"
            << "    netsh advfirewall firewall add rule name=\"Filedrop TCP\" dir=in action=allow protocol=TCP localport="
            << server.port() << "\n"
            << "    netsh advfirewall firewall add rule name=\"Filedrop UDP\" dir=in action=allow protocol=UDP localport="
            << dnsPort << "\n";
#endif
  std::cout << std::flush;

  if (openBrowser) {
    if (!util::openBrowser(app.adminUrl())) {
      std::cout << "未能自动打开浏览器，请手动访问上面的服务端界面地址。\n";
    }
  }

  std::thread inputThread([&app, &ip, &suffix, &options, port = server.port()] {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::string cmd = util::toLower(util::trim(line));
      if (cmd == "q" || cmd == "quit" || cmd == "exit") {
        g_stop = true;
        g_waitCv.notify_all();
        break;
      }
      if (cmd == "o" || cmd == "open") {
        util::openBrowser(app.adminUrl());
      } else if (cmd == "s" || cmd == "status") {
        std::cout << "监听端口 " << port << "，局域网地址 http://" << ip << suffix << "/，数据目录 "
                  << options.dataDir << "\n"
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
