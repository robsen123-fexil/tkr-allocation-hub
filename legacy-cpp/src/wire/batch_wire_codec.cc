#include "tkr/wire/batch_wire_codec.h"

#include "tkr/util/bounds.h"

#include <algorithm>
#include <cstring>

namespace tkr {
namespace wire {
namespace {

constexpr std::size_t kBatchHeaderSize = sizeof(WireBatchHeader);
constexpr std::size_t kBatchRecordSize = sizeof(WireBatchRecord);

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

void WriteU32Le(std::uint8_t* dst, std::uint32_t value) {
  dst[0] = static_cast<std::uint8_t>(value & 0xFFu);
  dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void WriteU16Le(std::uint8_t* dst, std::uint16_t value) {
  dst[0] = static_cast<std::uint8_t>(value & 0xFFu);
  dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

}  // namespace

BatchWireCodec::BatchWireCodec(BatchCodecConfig config)
    : config_(config), next_slot_id_(1) {}

void BatchWireCodec::AppendU32Le(std::vector<std::uint8_t>* out,
                                 std::uint32_t value) {
  std::uint8_t buf[4];
  WriteU32Le(buf, value);
  out->insert(out->end(), buf, buf + 4);
}

void BatchWireCodec::AppendRecordBytes(std::vector<std::uint8_t>* out,
                                       const WireBatchRecord& rec) {
  AppendU32Le(out, rec.record_id);
  AppendU32Le(out, rec.account_id);
  AppendU32Le(out, rec.symbol_id);
  AppendU32Le(out, rec.qty_milli);
  AppendU32Le(out, rec.price_tick);
  AppendU32Le(out, rec.payload_offset);
  AppendU32Le(out, rec.payload_len);
  AppendU32Le(out, rec.flags);
}

std::uint32_t BatchWireCodec::ComputeHeaderDigest(const WireBatchHeader& header) const {
  std::uint8_t buf[kBatchHeaderSize];
  WriteU32Le(buf + 0, header.magic);
  WriteU16Le(buf + 4, header.version);
  WriteU16Le(buf + 6, header.header_bytes);
  WriteU32Le(buf + 8, header.record_count);
  WriteU32Le(buf + 12, header.flags);
  WriteU32Le(buf + 16, header.desk_id);
  WriteU32Le(buf + 20, header.trade_date_yyyymmdd);
  WriteU32Le(buf + 24, 0);
  return util::Fnv1a32(buf, kBatchHeaderSize);
}

std::uint32_t BatchWireCodec::ComputePayloadDigest(const std::uint8_t* data,
                                                   std::size_t len) const {
  if (data == nullptr || len == 0) {
    return util::Fnv1a32(nullptr, 0);
  }
  return util::Fnv1a32(data, len);
}

Status BatchWireCodec::ValidateHeader(const WireBatchHeader& header) const {
  if (header.magic != kBatchMagic) {
    return Status::kInvalidMagic;
  }
  if (header.version != kWireVersion) {
    return Status::kBoundsError;
  }
  if (header.header_bytes < kBatchHeaderSize) {
    return Status::kTruncated;
  }
  if (header.record_count > config_.max_records ||
      header.record_count > kMaxBatchRecords) {
    return Status::kBoundsError;
  }
  if ((header.flags & kBatchFlagMarginCheck) != 0 && config_.validate_margin_flags) {
    if ((header.flags & kBatchFlagProRata) == 0) {
      return Status::kMarginBreach;
    }
  }
  return Status::kOk;
}

Status BatchWireCodec::ValidateRecordBounds(const WireBatchRecord& rec,
                                              std::size_t payload_size) const {
  if (!util::SliceInBounds(rec.payload_offset, rec.payload_len, payload_size)) {
    return Status::kBoundsError;
  }
  if (rec.qty_milli == 0 && (rec.flags & kBatchFlagPartialFill) == 0) {
    return Status::kComplianceReject;
  }
  return Status::kOk;
}

Status BatchWireCodec::ReadHeader(const std::uint8_t* data, std::size_t size,
                                  WireBatchHeader* out, std::size_t* consumed) {
  if (out == nullptr || data == nullptr || consumed == nullptr) {
    return Status::kBoundsError;
  }
  if (size < kBatchHeaderSize) {
    return Status::kTruncated;
  }
  out->magic = ReadU32Le(data + 0);
  out->version = ReadU16Le(data + 4);
  out->header_bytes = ReadU16Le(data + 6);
  out->record_count = ReadU32Le(data + 8);
  out->flags = ReadU32Le(data + 12);
  out->desk_id = ReadU32Le(data + 16);
  out->trade_date_yyyymmdd = ReadU32Le(data + 20);
  out->payload_digest = ReadU32Le(data + 24);
  *consumed = kBatchHeaderSize;
  return Status::kOk;
}

Status BatchWireCodec::ReadRecords(const std::uint8_t* data, std::size_t size,
                                   std::size_t offset, std::uint32_t count,
                                   std::vector<WireBatchRecord>* out) {
  if (out == nullptr || data == nullptr) {
    return Status::kBoundsError;
  }
  const std::size_t records_bytes = static_cast<std::size_t>(count) * kBatchRecordSize;
  if (!util::SectionBodyInBounds(offset, records_bytes, size)) {
    return Status::kBoundsError;
  }
  out->clear();
  out->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t base = offset + static_cast<std::size_t>(i) * kBatchRecordSize;
    WireBatchRecord rec{};
    rec.record_id = ReadU32Le(data + base + 0);
    rec.account_id = ReadU32Le(data + base + 4);
    rec.symbol_id = ReadU32Le(data + base + 8);
    rec.qty_milli = ReadU32Le(data + base + 12);
    rec.price_tick = ReadU32Le(data + base + 16);
    rec.payload_offset = ReadU32Le(data + base + 20);
    rec.payload_len = ReadU32Le(data + base + 24);
    rec.flags = ReadU32Le(data + base + 28);
    out->push_back(rec);
  }
  return Status::kOk;
}

Status BatchWireCodec::ReadPayloadBlob(const std::uint8_t* data, std::size_t size,
                                       std::size_t offset,
                                       std::vector<std::uint8_t>* out) {
  if (out == nullptr || data == nullptr) {
    return Status::kBoundsError;
  }
  if (offset >= size) {
    out->clear();
    return Status::kOk;
  }
  const std::size_t len = size - offset;
  out->assign(data + offset, data + size);
  return Status::kOk;
}

BatchEncodeResult BatchWireCodec::EncodeBatch(
    const WireBatchHeader& header, const std::vector<WireBatchRecord>& records,
    const std::uint8_t* payload_data, std::size_t payload_size) {
  BatchEncodeResult result{};
  result.status = Status::kOk;

  WireBatchHeader hdr = header;
  hdr.magic = kBatchMagic;
  hdr.version = kWireVersion;
  hdr.header_bytes = static_cast<std::uint16_t>(kBatchHeaderSize);
  hdr.record_count = static_cast<std::uint32_t>(records.size());
  hdr.payload_digest = ComputePayloadDigest(payload_data, payload_size);

  result.status = ValidateHeader(hdr);
  if (result.status != Status::kOk) {
    return result;
  }

  for (const WireBatchRecord& rec : records) {
    result.status = ValidateRecordBounds(rec, payload_size);
    if (result.status != Status::kOk) {
      return result;
    }
  }

  std::vector<std::uint8_t>& out = result.wire_bytes;
  out.reserve(kBatchHeaderSize + records.size() * kBatchRecordSize + payload_size);

  AppendU32Le(&out, hdr.magic);
  std::uint8_t ver_buf[2];
  WriteU16Le(ver_buf, hdr.version);
  out.insert(out.end(), ver_buf, ver_buf + 2);
  WriteU16Le(ver_buf, hdr.header_bytes);
  out.insert(out.end(), ver_buf, ver_buf + 2);
  AppendU32Le(&out, hdr.record_count);
  AppendU32Le(&out, hdr.flags);
  AppendU32Le(&out, hdr.desk_id);
  AppendU32Le(&out, hdr.trade_date_yyyymmdd);
  AppendU32Le(&out, hdr.payload_digest);

  const std::size_t record_table_offset = out.size();
  for (const WireBatchRecord& rec : records) {
    AppendRecordBytes(&out, rec);
  }

  const std::size_t payload_offset = out.size();
  if (payload_data != nullptr && payload_size > 0) {
    out.insert(out.end(), payload_data, payload_data + payload_size);
  }

  BatchWireFrame staging_frame;
  staging_frame.header = hdr;
  staging_frame.records = records;
  staging_frame.payload_blob.assign(out.begin() + payload_offset, out.end());

  if (config_.enable_deferred_staging) {
    deferred_table_.clear();
    next_slot_id_ = 1;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const WireBatchRecord& rec = records[i];
      if (rec.payload_len == 0) {
        continue;
      }
      DeferredSlot slot{};
      slot.slot_id = next_slot_id_++;
      slot.record_id = rec.record_id;
      slot.payload_len = rec.payload_len;
      slot.staging_flags = rec.flags;
      slot.active = true;
      if (payload_data != nullptr &&
          util::SliceInBounds(rec.payload_offset, rec.payload_len, payload_size)) {
        slot.payload_ptr = payload_data + rec.payload_offset;
      } else if (util::SliceInBounds(rec.payload_offset, rec.payload_len,
                                     staging_frame.payload_blob.size())) {
        slot.payload_ptr = staging_frame.payload_blob.data() + rec.payload_offset;
      } else {
        slot.active = false;
      }
      deferred_table_.push_back(slot);
    }
    result.deferred_slot_count = deferred_table_.size();
  }

  result.payload_digest = hdr.payload_digest;
  return result;
}

BatchDecodeResult BatchWireCodec::DecodeBatch(const std::uint8_t* data,
                                              std::size_t size) {
  BatchDecodeResult result{};
  result.status = Status::kOk;
  result.consumed_bytes = 0;

  if (data == nullptr || size == 0) {
    result.status = Status::kTruncated;
    return result;
  }

  std::size_t consumed = 0;
  result.status = ReadHeader(data, size, &result.frame.header, &consumed);
  if (result.status != Status::kOk) {
    return result;
  }

  result.status = ValidateHeader(result.frame.header);
  if (result.status != Status::kOk) {
    return result;
  }

  const std::uint32_t count = result.frame.header.record_count;
  const std::size_t records_offset = consumed;
  result.status =
      ReadRecords(data, size, records_offset, count, &result.frame.records);
  if (result.status != Status::kOk) {
    return result;
  }
  consumed += static_cast<std::size_t>(count) * kBatchRecordSize;

  std::size_t payload_span = 0;
  for (const WireBatchRecord& rec : result.frame.records) {
    if (rec.payload_len == 0) {
      continue;
    }
    const std::size_t rec_end =
        static_cast<std::size_t>(rec.payload_offset + rec.payload_len);
    if (rec_end > payload_span) {
      payload_span = rec_end;
    }
  }

  if (!util::SectionBodyInBounds(consumed, payload_span, size)) {
    result.status = Status::kBoundsError;
    return result;
  }

  result.frame.payload_blob.assign(data + consumed, data + consumed + payload_span);
  consumed += payload_span;

  for (const WireBatchRecord& rec : result.frame.records) {
    result.status =
        ValidateRecordBounds(rec, result.frame.payload_blob.size());
    if (result.status != Status::kOk) {
      return result;
    }
  }

  const std::uint32_t expected_digest =
      ComputePayloadDigest(result.frame.payload_blob.data(),
                           result.frame.payload_blob.size());
  if (result.frame.header.payload_digest != 0 &&
      result.frame.header.payload_digest != expected_digest) {
    result.status = Status::kBoundsError;
    return result;
  }

  if (config_.enable_deferred_staging) {
    result.status = StageDeferredSlots(&result.frame);
    if (result.status != Status::kOk) {
      return result;
    }
  }

  result.consumed_bytes = consumed;
  return result;
}

Status BatchWireCodec::StageDeferredSlots(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }
  frame->deferred_slots.clear();
  deferred_table_.clear();
  next_slot_id_ = 1;

  for (const WireBatchRecord& rec : frame->records) {
    if (rec.payload_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(rec.payload_offset, rec.payload_len,
                             frame->payload_blob.size())) {
      return Status::kBoundsError;
    }
    DeferredSlot slot{};
    slot.slot_id = next_slot_id_++;
    slot.record_id = rec.record_id;
    slot.payload_ptr = frame->payload_blob.data() + rec.payload_offset;
    slot.payload_len = rec.payload_len;
    slot.staging_flags = rec.flags;
    slot.active = true;
    frame->deferred_slots.push_back(slot);
    deferred_table_.push_back(slot);
    if (deferred_table_.size() > kMaxDeferredSlots) {
      return Status::kBoundsError;
    }
  }
  return Status::kOk;
}

Status BatchWireCodec::ClearDeferredSlots(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }
  for (DeferredSlot& slot : frame->deferred_slots) {
    slot.payload_ptr = nullptr;
    slot.active = false;
  }
  for (DeferredSlot& slot : deferred_table_) {
    slot.payload_ptr = nullptr;
    slot.active = false;
  }
  frame->deferred_slots.clear();
  deferred_table_.clear();
  return Status::kOk;
}

}  // namespace wire
}  // namespace tkr
