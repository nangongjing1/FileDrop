#include <cstdio>
#include <string>

#include "http_server.h"
#include "util.h"

namespace {

int failures = 0;

void check(bool condition, const char* label) {
  if (!condition) {
    std::printf("FAIL: %s\n", label);
    ++failures;
  }
}

void checkEqual(const std::string& actual, const std::string& expected, const char* label) {
  if (actual != expected) {
    std::printf("FAIL: %s (got \"%s\", want \"%s\")\n", label, actual.c_str(), expected.c_str());
    ++failures;
  }
}

}  // namespace

int main() {
  {
    HttpRequest request;
    const std::string head =
        "GET /api/client/state?deviceId=abc&name=%E4%B8%AD%E6%96%87 HTTP/1.1\r\n"
        "Host: filedrop.ng\r\n"
        "X-Device-Id: abc\r\n"
        "x-device-id: def\r\n"
        "Content-Length: 42\r\n"
        "Transfer-Encoding: \r\n";
    check(httpParseRequestHead(head, request) == HeadParseResult::Ok, "parse ok");
    checkEqual(request.method, "GET", "method");
    checkEqual(request.path, "/api/client/state", "path");
    checkEqual(request.param("deviceId"), "abc", "query parameter");
    checkEqual(request.param("name"), "中文", "query parameter is utf-8 decoded");
    checkEqual(request.header("host"), "filedrop.ng", "host header lower case");
    checkEqual(request.header("x-device-id"), "abc, def", "duplicate header merged");
    check(request.contentLength == 42, "content length parsed");
  }

  {
    HttpRequest request;
    const std::string head = "POST /api/admin/send HTTP/1.1\r\nTransfer-Encoding: chunked\r\n";
    check(httpParseRequestHead(head, request) == HeadParseResult::ChunkedNotSupported, "chunked rejected");
  }

  {
    HttpRequest request;
    check(httpParseRequestHead("BADLINE\r\n\r\n", request) == HeadParseResult::BadRequest, "bad request line");
    check(httpParseRequestHead("", request) == HeadParseResult::BadRequest, "empty request");
  }

  {
    HttpRequest request;
    check(httpParseRequestHead("GET /x HTTP/1.1\r\nContent-Length: abc\r\n", request) ==
              HeadParseResult::BadRequest,
          "invalid content length");
  }

  {
    HttpRequest request;
    check(httpParseRequestHead("PUT /api/client/upload?path=a%2Fb.txt HTTP/1.1\r\n", request) ==
              HeadParseResult::Ok,
          "upload parse ok");
    checkEqual(request.path, "/api/client/upload", "upload path");
    checkEqual(request.param("path"), "a/b.txt", "encoded path value");
  }

  checkEqual(util::sanitizeName("..\\..\\etc\\passwd"), "passwd", "sanitize keeps base name");
  checkEqual(util::sanitizeName("测试 文件.txt"), "测试 文件.txt", "sanitize keeps chinese");
  checkEqual(util::sanitizeName("a<b>c:d"), "a_b_c_d", "sanitize replaces reserved chars");
  checkEqual(util::sanitizeName("   "), "unnamed", "sanitize empty");
  checkEqual(util::sanitizeName("..."), "unnamed", "sanitize dots");

  checkEqual(util::normalizeRelPath("../../etc/测试 文件.txt"), "etc/测试 文件.txt", "relative path traversal");
  checkEqual(util::normalizeRelPath("a/b/../c.txt"), "a/b/c.txt", "relative path dots");
  checkEqual(util::normalizeRelPath("dir\\sub\\file.txt"), "dir/sub/file.txt", "backslashes normalized");
  checkEqual(util::normalizeRelPath("/absolute/path.txt"), "absolute/path.txt", "leading slash removed");

  checkEqual(util::urlDecode(util::urlEncode("a b+c/中文.txt")), "a b+c/中文.txt", "url encode roundtrip");

  checkEqual(util::prefixToNetmask(24), "255.255.255.0", "netmask /24");
  checkEqual(util::prefixToNetmask(8), "255.0.0.0", "netmask /8");
  checkEqual(util::prefixToNetmask(0), "0.0.0.0", "netmask /0");
  checkEqual(util::prefixToNetmask(32), "255.255.255.255", "netmask /32");

  check(util::isPrivateIPv4("192.168.1.10"), "private 192.168");
  check(util::isPrivateIPv4("10.142.251.160"), "private 10.");
  check(util::isPrivateIPv4("172.20.1.1"), "private 172.16-31");
  check(!util::isPrivateIPv4("172.32.1.1"), "public 172.32");
  check(!util::isPrivateIPv4("8.8.8.8"), "public 8.8.8.8");

  uint32_t ip = 0;
  check(util::parseIPv4("10.0.0.1", ip) && ip == 0x0A000001u, "parse ipv4");

  if (failures == 0) {
    std::printf("HTTP/util tests passed\n");
    return 0;
  }
  std::printf("HTTP/util tests failed: %d\n", failures);
  return 1;
}
