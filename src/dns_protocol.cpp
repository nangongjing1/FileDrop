#include "dns_protocol.h"

#include <cctype>
#include <cstring>

namespace dnsproto {
namespace {

bool readName(const std::vector<uint8_t>& data, size_t offset, std::string& name, size_t& endOffset) {
  name.clear();
  size_t pos = offset;
  int guard = 0;
  while (pos < data.size()) {
    if (++guard > 64) return false;
    uint8_t len = data[pos];
    if (len == 0) {
      ++pos;
      endOffset = pos;
      return !name.empty();
    }
    if (len & 0xC0) return false;  // 查询的问题段不应出现压缩指针
    if (pos + 1 + len > data.size()) return false;
    if (!name.empty()) name.push_back('.');
    name.append(reinterpret_cast<const char*>(data.data() + pos + 1), len);
    pos += 1 + len;
  }
  return false;
}

void put16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value >> 8);
  out[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

}  // namespace

std::string normalizeDomain(const std::string& input) {
  std::string domain;
  domain.reserve(input.size());
  for (unsigned char c : input) {
    domain.push_back(static_cast<char>(std::tolower(c)));
  }
  while (!domain.empty() && domain.back() == '.') domain.pop_back();
  return domain;
}

bool parseQuestion(const std::vector<uint8_t>& query, std::string& name, uint16_t& qtype, size_t& questionEnd) {
  if (query.size() < 12) return false;
  size_t end = 0;
  if (!readName(query, 12, name, end)) return false;
  if (end + 4 > query.size()) return false;
  qtype = static_cast<uint16_t>((query[end] << 8) | query[end + 1]);
  questionEnd = end + 4;
  return true;
}

std::vector<uint8_t> buildAnswer(const std::vector<uint8_t>& query, uint16_t qtype, uint32_t answerIp) {
  std::string name;
  uint16_t parsedType = 0;
  size_t questionEnd = 0;
  if (!parseQuestion(query, name, parsedType, questionEnd)) return {};
  (void)parsedType;

  size_t questionLength = questionEnd - 12;
  bool answerA = qtype == 1;
  std::vector<uint8_t> response(12 + questionLength + (answerA ? 16 : 0), 0);

  response[0] = query[0];
  response[1] = query[1];
  response[2] = 0x81;  // QR + RD
  response[3] = 0x80;  // RA
  put16(response, 4, 1);
  put16(response, 6, answerA ? 1 : 0);
  put16(response, 8, 0);
  put16(response, 10, 0);
  std::memcpy(response.data() + 12, query.data() + 12, questionLength);

  if (answerA) {
    size_t offset = 12 + questionLength;
    response[offset] = 0xC0;
    response[offset + 1] = 0x0C;  // 指向问题段的域名
    put16(response, offset + 2, 1);
    put16(response, offset + 4, 1);
    response[offset + 6] = 0;
    response[offset + 7] = 0;
    response[offset + 8] = 0;
    response[offset + 9] = 60;
    put16(response, offset + 10, 4);
    std::memcpy(response.data() + offset + 12, &answerIp, 4);
  }
  return response;
}

std::vector<uint8_t> buildFailure(const std::vector<uint8_t>& query, uint8_t rcode) {
  std::string name;
  uint16_t qtype = 0;
  size_t questionEnd = 0;
  if (!parseQuestion(query, name, qtype, questionEnd)) return {};

  size_t questionLength = questionEnd - 12;
  std::vector<uint8_t> response(12 + questionLength, 0);
  response[0] = query[0];
  response[1] = query[1];
  response[2] = 0x81;
  response[3] = static_cast<uint8_t>(0x80 | (rcode & 0x0F));
  put16(response, 4, 1);
  put16(response, 6, 0);
  put16(response, 8, 0);
  put16(response, 10, 0);
  std::memcpy(response.data() + 12, query.data() + 12, questionLength);
  return response;
}

}  // namespace dnsproto
