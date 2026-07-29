#include "tkr/wire/wire_validator.h"

#include "tkr/util/bounds.h"

namespace tkr {
namespace wire {
namespace {

std::uint32_t ReadU32Le(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint16_t ReadU16Le(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8);
}

}  // namespace

WireValidator::WireValidator(bool strict_digest, std::uint32_t max_records)
    : strict_digest_(strict_digest), max_records_(max_records) {}

ValidationResult WireValidator::ValidateBatchFrame(const BatchWireFrame& frame) {
  ValidationResult result{};
  result.status = Status::kOk;

  if (frame.header.magic != kBatchMagic) {
    result.status = Status::kInvalidMagic;
    AddIssue(&result, "bad_magic", "Expected TKR1 magic");
    return result;
  }

  if (frame.header.version != kWireVersion) {
    result.status = Status::kBoundsError;
    AddIssue(&result, "bad_version", "Unsupported wire version");
  }

  if (frame.records.size() != frame.header.record_count) {
    result.status = Status::kBoundsError;
    AddIssue(&result, "record_count_mismatch", "Header count differs from table");
  }

  if (frame.records.size() > max_records_) {
    result.status = Status::kBoundsError;
    AddIssue(&result, "too_many_records", "Record count exceeds limit");
  }

  result.payload_bytes_checked =
      static_cast<std::int32_t>(frame.payload_blob.size());

  for (std::size_t i = 0; i < frame.records.size(); ++i) {
    ++result.records_checked;
    ValidateRecord(&result, frame.records[i], frame.payload_blob,
                   static_cast<std::int32_t>(i));
  }

  if (strict_digest_ && frame.header.payload_digest != 0) {
    const std::uint32_t computed =
        util::Fnv1a32(frame.payload_blob.data(), frame.payload_blob.size());
    if (computed != frame.header.payload_digest) {
      result.status = Status::kBoundsError;
      AddIssue(&result, "digest_mismatch", "Payload digest mismatch");
    }
  }

  if (!result.issues.empty() && result.status == Status::kOk) {
    result.status = Status::kBoundsError;
  }
  return result;
}

ValidationResult WireValidator::ValidateBatchBytes(const std::uint8_t* data,
                                                     std::size_t len) {
  ValidationResult result{};
  result.status = Status::kOk;
  if (data == nullptr || len < 32) {
    result.status = Status::kTruncated;
    AddIssue(&result, "truncated", "Batch frame too short");
    return result;
  }

  if (ReadU32Le(data) != kBatchMagic) {
    result.status = Status::kInvalidMagic;
    AddIssue(&result, "bad_magic", "Not a TKR1 frame");
    return result;
  }

  const std::uint32_t record_count = ReadU32Le(data + 8);
  if (record_count > max_records_) {
    result.status = Status::kBoundsError;
    AddIssue(&result, "bad_record_count", "Invalid record count");
    return result;
  }

  const std::size_t table_bytes = static_cast<std::size_t>(record_count) * 32;
  if (!util::SectionBodyInBounds(32, table_bytes, len)) {
    result.status = Status::kTruncated;
    AddIssue(&result, "truncated_table", "Record table exceeds frame");
    return result;
  }

  result.records_checked = static_cast<std::int32_t>(record_count);
  result.payload_bytes_checked =
      static_cast<std::int32_t>(len - 32 - table_bytes);
  return result;
}

ValidationResult WireValidator::ValidateEnvelopeBytes(const std::uint8_t* data,
                                                      std::size_t len) {
  ValidationResult result{};
  result.status = Status::kOk;
  if (data == nullptr || len < 24) {
    result.status = Status::kTruncated;
    AddIssue(&result, "truncated", "Envelope too short");
    return result;
  }

  if (ReadU32Le(data) != kEnvelopeMagic) {
    result.status = Status::kInvalidMagic;
    AddIssue(&result, "bad_magic", "Not a TKR2 envelope");
    return result;
  }

  const std::uint16_t channel_count = ReadU16Le(data + 6);
  if (channel_count > kMaxEnvelopeChannels) {
    result.status = Status::kBoundsError;
    AddIssue(&result, "too_many_channels", "Channel count exceeds max");
  }
  return result;
}

ValidationResult WireValidator::ValidateSessionBytes(const std::uint8_t* data,
                                                     std::size_t len) {
  ValidationResult result{};
  result.status = Status::kOk;
  if (data == nullptr || len < 24) {
    result.status = Status::kTruncated;
    AddIssue(&result, "truncated", "Session frame too short");
    return result;
  }

  if (ReadU32Le(data) != kSessionMagic) {
    result.status = Status::kInvalidMagic;
    AddIssue(&result, "bad_magic", "Not a TKR3 session");
    return result;
  }

  const std::uint16_t leg_count = ReadU16Le(data + 6);
  if (leg_count > kMaxSessionLegs) {
    result.status = Status::kBoundsError;
    AddIssue(&result, "too_many_legs", "Leg count exceeds max");
  }
  return result;
}

void WireValidator::ValidateRecord(ValidationResult* result,
                                   const WireBatchRecord& rec,
                                   const std::vector<std::uint8_t>& payload,
                                   std::int32_t index) {
  if (result == nullptr) {
    return;
  }
  if (rec.qty_milli == 0 && rec.payload_len > 0) {
    AddIssue(result, "zero_qty_payload", "Zero qty with payload", index);
  }
  if (rec.payload_len > 0 &&
      !util::SliceInBounds(rec.payload_offset, rec.payload_len, payload.size())) {
    result->status = Status::kBoundsError;
    AddIssue(result, "payload_oob", "Payload out of bounds", index);
  }
  if (rec.price_tick < 0) {
    AddIssue(result, "negative_price", "Negative price tick", index);
  }
  if ((rec.flags & kBatchFlagMarginCheck) != 0 &&
      (rec.flags & kBatchFlagProRata) == 0) {
    AddIssue(result, "margin_without_prorata", "Margin flag without pro-rata", index);
  }
}

void WireValidator::AddIssue(ValidationResult* result, const char* code,
                             const char* detail, std::int32_t record_index) {
  if (result == nullptr) {
    return;
  }
  ValidationIssue issue{};
  issue.code = code;
  issue.detail = detail;
  issue.record_index = record_index;
  result->issues.push_back(issue);
}

}  // namespace wire
}  // namespace tkr
