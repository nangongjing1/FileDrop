#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dnsproto {

constexpr size_t kMaxQuery = 2048;

std::string normalizeDomain(const std::string& input);

// 解析 DNS 查询的问题段，questionEnd 指向 QCLASS 之后的偏移。
bool parseQuestion(const std::vector<uint8_t>& query, std::string& name, uint16_t& qtype, size_t& questionEnd);

// 为 A 查询构造带一条 A 记录的应答；其它类型返回无记录应答；非法查询返回空。
std::vector<uint8_t> buildAnswer(const std::vector<uint8_t>& query, uint16_t qtype, uint32_t answerIp);

// 构造不携带记录的失败应答（保留问题段）。
std::vector<uint8_t> buildFailure(const std::vector<uint8_t>& query, uint8_t rcode);

}  // namespace dnsproto
