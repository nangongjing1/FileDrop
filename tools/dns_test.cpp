#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dns_protocol.h"

namespace {

int failures = 0;

void check(bool condition, const char* label) {
  if (!condition) {
    std::printf("FAIL: %s\n", label);
    ++failures;
  }
}

uint16_t read16(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

std::vector<uint8_t> makeQuery(const std::string& name, uint16_t qtype) {
  std::vector<uint8_t> out = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  size_t start = 0;
  while (start < name.size()) {
    size_t dot = name.find('.', start);
    if (dot == std::string::npos) dot = name.size();
    std::string label = name.substr(start, dot - start);
    out.push_back(static_cast<uint8_t>(label.size()));
    out.insert(out.end(), label.begin(), label.end());
    start = dot + 1;
  }
  out.push_back(0);
  out.push_back(static_cast<uint8_t>(qtype >> 8));
  out.push_back(static_cast<uint8_t>(qtype & 0xFF));
  out.push_back(0x00);
  out.push_back(0x01);
  return out;
}

}  // namespace

int main() {
  auto query = makeQuery("filedrop.ng", 1);
  std::string name;
  uint16_t type = 0;
  size_t questionEnd = 0;

  check(dnsproto::parseQuestion(query, name, type, questionEnd), "parse A question");
  check(name == "filedrop.ng", "question name");
  check(type == 1, "question type");
  check(questionEnd == query.size(), "question length");

  uint8_t ipBytes[4] = {10, 0, 0, 5};
  uint32_t ip = 0;
  std::memcpy(&ip, ipBytes, sizeof ip);

  auto answer = dnsproto::buildAnswer(query, 1, ip);
  check(answer.size() == query.size() + 16, "answer size");
  check(read16(answer, 4) == 1, "answer qdcount");
  check(read16(answer, 6) == 1, "answer ancount");
  check((answer[2] & 0x80) != 0, "answer QR flag");
  check((answer[3] & 0x0F) == 0, "answer rcode");
  size_t record = query.size();
  check(answer[record] == 0xC0 && answer[record + 1] == 0x0C, "answer name pointer");
  check(read16(answer, record + 2) == 1, "answer A type");
  check(read16(answer, record + 4) == 1, "answer IN class");
  check(read16(answer, record + 10) == 4, "answer rdlength");
  check(std::memcmp(answer.data() + record + 12, ipBytes, 4) == 0, "answer address bytes");

  auto aaaaQuery = makeQuery("filedrop.ng", 28);
  auto aaaaAnswer = dnsproto::buildAnswer(aaaaQuery, 28, ip);
  check(aaaaAnswer.size() == aaaaQuery.size(), "AAAA answer size");
  check(read16(aaaaAnswer, 6) == 0, "AAAA ancount");

  auto failure = dnsproto::buildFailure(query, 2);
  check(failure.size() == query.size(), "failure size");
  check((failure[3] & 0x0F) == 2, "failure rcode");
  check(std::memcmp(failure.data() + 12, query.data() + 12, query.size() - 12) == 0, "failure keeps question");

  check(dnsproto::normalizeDomain("FileDrop.NG.") == "filedrop.ng", "normalize domain");
  check(dnsproto::normalizeDomain("filedrop.ng") == "filedrop.ng", "normalize exact");

  std::vector<uint8_t> truncated = {0x12, 0x34, 0x01};
  check(!dnsproto::parseQuestion(truncated, name, type, questionEnd), "truncated parse");
  check(dnsproto::buildAnswer(truncated, 1, ip).empty(), "truncated answer");
  check(dnsproto::buildFailure(truncated, 2).empty(), "truncated failure");

  if (failures == 0) {
    std::printf("DNS protocol tests passed\n");
    return 0;
  }
  std::printf("DNS protocol tests failed: %d\n", failures);
  return 1;
}
