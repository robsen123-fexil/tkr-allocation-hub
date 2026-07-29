#pragma once

#include "tkr/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tkr {
namespace wire {

struct ValidationIssue {
  std::string code;
  std::string detail;
  std::int32_t record_index;
};

struct ValidationResult {
  Status status;
  std::vector<ValidationIssue> issues;
  std::int32_t records_checked;
  std::int32_t payload_bytes_checked;
};

class WireValidator {
 public:
  WireValidator(bool strict_digest, std::uint32_t max_records);

  ValidationResult ValidateBatchFrame(const BatchWireFrame& frame);
  ValidationResult ValidateBatchBytes(const std::uint8_t* data, std::size_t len);
  ValidationResult ValidateEnvelopeBytes(const std::uint8_t* data, std::size_t len);
  ValidationResult ValidateSessionBytes(const std::uint8_t* data, std::size_t len);

 private:
  void ValidateRecord(ValidationResult* result, const WireBatchRecord& rec,
                      const std::vector<std::uint8_t>& payload,
                      std::int32_t index);
  static void AddIssue(ValidationResult* result, const char* code,
                     const char* detail, std::int32_t record_index = -1);

  bool strict_digest_;
  std::uint32_t max_records_;
};

}  // namespace wire
}  // namespace tkr
