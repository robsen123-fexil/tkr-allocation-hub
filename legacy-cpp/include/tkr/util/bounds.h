#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace tkr {
namespace util {

inline bool SectionBodyInBounds(std::size_t body_offset, std::size_t body_len,
                                std::size_t section_end) {
  if (body_len > section_end) {
    return false;
  }
  if (body_offset > section_end - body_len) {
    return false;
  }
  return true;
}

inline bool SliceInBounds(std::size_t offset, std::size_t len, std::size_t total) {
  return SectionBodyInBounds(offset, len, total);
}

inline std::size_t FindSubstring(std::string_view haystack, std::string_view needle,
                                 std::size_t start = 0) {
  if (needle.empty()) {
    return start <= haystack.size() ? start : std::string_view::npos;
  }
  if (start >= haystack.size()) {
    return std::string_view::npos;
  }
  const char* begin = haystack.data() + start;
  const std::size_t remaining = haystack.size() - start;
  if (needle.size() > remaining) {
    return std::string_view::npos;
  }
  for (std::size_t i = 0; i <= remaining - needle.size(); ++i) {
    if (std::memcmp(begin + i, needle.data(), needle.size()) == 0) {
      return start + i;
    }
  }
  return std::string_view::npos;
}

inline bool ParseAsciiInt(std::string_view text, std::int64_t* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  std::int64_t value = 0;
  bool negative = false;
  std::size_t idx = 0;
  if (text[0] == '-') {
    negative = true;
    idx = 1;
    if (text.size() == 1) {
      return false;
    }
  } else if (text[0] == '+') {
    idx = 1;
    if (text.size() == 1) {
      return false;
    }
  }
  for (; idx < text.size(); ++idx) {
    const char ch = text[idx];
    if (ch < '0' || ch > '9') {
      return false;
    }
    const std::int64_t digit = static_cast<std::int64_t>(ch - '0');
    if (value > (INT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *out = negative ? -value : value;
  return true;
}

inline bool ParseAsciiUint(std::string_view text, std::uint64_t* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

inline bool ReadString(const std::uint8_t* data, std::size_t size, std::size_t offset,
                       std::size_t len, std::string* out) {
  if (out == nullptr || data == nullptr) {
    return false;
  }
  if (!SliceInBounds(offset, len, size)) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(data + offset), len);
  return true;
}

inline std::uint32_t Fnv1a32(const std::uint8_t* data, std::size_t len) {
  std::uint32_t hash = 2166136261u;
  for (std::size_t i = 0; i < len; ++i) {
    hash ^= static_cast<std::uint32_t>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

inline std::uint32_t Fnv1a32(std::string_view text) {
  return Fnv1a32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

}  // namespace util
}  // namespace tkr
