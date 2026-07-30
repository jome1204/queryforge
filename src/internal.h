#ifndef QUERYFORGE_INTERNAL_H
#define QUERYFORGE_INTERNAL_H

#include "queryforge/database.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace queryforge::internal {

inline bool fail(Error& error, ErrorCode code, uint64_t offset,
                 std::string message) {
  error.code = code;
  error.offset = offset;
  error.message = std::move(message);
  return false;
}

template <typename T>
bool checked_add(T left, T right, T& result) {
  static_assert(std::numeric_limits<T>::is_integer, "integer required");
  if (right > std::numeric_limits<T>::max() - left) return false;
  result = static_cast<T>(left + right);
  return true;
}

template <typename T>
bool checked_multiply(T left, T right, T& result) {
  static_assert(std::numeric_limits<T>::is_integer, "integer required");
  if (left != 0 && right > std::numeric_limits<T>::max() / left) return false;
  result = static_cast<T>(left * right);
  return true;
}

inline std::string normalize(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (unsigned char c : value) {
    if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
    output.push_back(static_cast<char>(c));
  }
  return output;
}

inline uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) {
  return static_cast<uint64_t>(le32(p)) |
         (static_cast<uint64_t>(le32(p + 4)) << 32);
}
inline void append16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}
inline void append32(std::vector<uint8_t>& out, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}
inline void append64(std::vector<uint8_t>& out, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    out.push_back(static_cast<uint8_t>(value >> shift));
}
inline void patch32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out[offset + shift / 8] = static_cast<uint8_t>(value >> shift);
}

inline std::unique_ptr<Expression> clone(const std::unique_ptr<Expression>& p) {
  return p ? std::make_unique<Expression>(*p) : nullptr;
}

}  // namespace queryforge::internal
#endif
