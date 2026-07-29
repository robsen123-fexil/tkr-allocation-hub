#include "tkr/wire/session_wire_codec.h"

#include "tkr/util/bounds.h"

#include <algorithm>
#include <cstring>

namespace tkr {
namespace wire {
namespace {

constexpr std::size_t kSessionHeaderSize = 24;
constexpr std::size_t kSessionLegSize = 28;

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

SessionWireCodec::SessionWireCodec(SessionCodecConfig config)
    : config_(config), next_slot_id_(1) {}

void SessionWireCodec::AppendU32Le(std::vector<std::uint8_t>* out,
                                 std::uint32_t value) {
  std::uint8_t buf[4];
  WriteU32Le(buf, value);
  out->insert(out->end(), buf, buf + 4);
}

void SessionWireCodec::AppendU16Le(std::vector<std::uint8_t>* out,
                                   std::uint16_t value) {
  std::uint8_t buf[2];
  WriteU16Le(buf, value);
  out->insert(out->end(), buf, buf + 2);
}

void SessionWireCodec::AppendLegBytes(std::vector<std::uint8_t>* out,
                                      const WireSessionLeg& leg) {
  AppendU32Le(out, leg.leg_id);
  AppendU32Le(out, leg.cl_ord_id);
  AppendU32Le(out, leg.alloc_account);
  AppendU32Le(out, leg.qty_milli);
  AppendU32Le(out, leg.ref_offset);
  AppendU32Le(out, leg.ref_len);
  AppendU32Le(out, leg.flags);
}

std::uint32_t SessionWireCodec::ComputeHeaderDigest(
    const WireSessionHeader& header) const {
  std::uint8_t buf[kSessionHeaderSize];
  WriteU32Le(buf + 0, header.magic);
  WriteU16Le(buf + 4, header.version);
  WriteU16Le(buf + 6, header.leg_count);
  WriteU32Le(buf + 8, header.session_id);
  WriteU32Le(buf + 12, header.checkpoint_seq);
  WriteU32Le(buf + 16, header.flags);
  WriteU32Le(buf + 20, 0);
  return util::Fnv1a32(buf, kSessionHeaderSize);
}

std::uint32_t SessionWireCodec::ComputeRefDigest(const std::uint8_t* data,
                                                 std::size_t len) const {
  if (data == nullptr || len == 0) {
    return util::Fnv1a32(nullptr, 0);
  }
  return util::Fnv1a32(data, len);
}

std::uint32_t SessionWireCodec::ComputeLegTableDigest(
    const std::vector<WireSessionLeg>& legs) const {
  std::uint32_t digest = 2166136261u;
  for (const WireSessionLeg& leg : legs) {
    digest ^= leg.leg_id;
    digest *= 16777619u;
    digest ^= leg.alloc_account;
    digest *= 16777619u;
    digest ^= leg.qty_milli;
    digest *= 16777619u;
  }
  return digest;
}

Status SessionWireCodec::ValidateHeader(const WireSessionHeader& header) const {
  if (header.magic != kSessionMagic) {
    return Status::kInvalidMagic;
  }
  if (header.version != kWireVersion) {
    return Status::kBoundsError;
  }
  if (header.leg_count > config_.max_legs ||
      header.leg_count > kMaxSessionLegs) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status SessionWireCodec::ValidateLegBounds(const WireSessionLeg& leg,
                                           std::size_t ref_size) const {
  if (leg.ref_len == 0) {
    return Status::kOk;
  }
  if (!util::SliceInBounds(leg.ref_offset, leg.ref_len, ref_size)) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status SessionWireCodec::ValidateLegRefs(const SessionWireFrame& frame) const {
  for (const WireSessionLeg& leg : frame.legs) {
    Status st = ValidateLegBounds(leg, frame.ref_blob.size());
    if (st != Status::kOk) {
      return st;
    }
  }
  return Status::kOk;
}

Status SessionWireCodec::ReadHeader(const std::uint8_t* data, std::size_t size,
                                    WireSessionHeader* out,
                                    std::size_t* consumed) {
  if (out == nullptr || data == nullptr || size < kSessionHeaderSize) {
    return Status::kTruncated;
  }

  out->magic = ReadU32Le(data + 0);
  out->version = ReadU16Le(data + 4);
  out->leg_count = ReadU16Le(data + 6);
  out->session_id = ReadU32Le(data + 8);
  out->checkpoint_seq = ReadU32Le(data + 12);
  out->flags = ReadU32Le(data + 16);
  out->merge_digest = ReadU32Le(data + 20);

  if (consumed != nullptr) {
    *consumed = kSessionHeaderSize;
  }
  return Status::kOk;
}

Status SessionWireCodec::ReadLegs(const std::uint8_t* data, std::size_t size,
                                  std::size_t offset, std::uint16_t count,
                                  std::vector<WireSessionLeg>* out) {
  if (out == nullptr || data == nullptr) {
    return Status::kBoundsError;
  }

  const std::size_t table_bytes = static_cast<std::size_t>(count) * kSessionLegSize;
  if (!util::SectionBodyInBounds(offset, table_bytes, size)) {
    return Status::kBoundsError;
  }

  out->clear();
  out->reserve(count);
  std::size_t cursor = offset;

  for (std::uint16_t i = 0; i < count; ++i) {
    WireSessionLeg leg{};
    leg.leg_id = ReadU32Le(data + cursor + 0);
    leg.cl_ord_id = ReadU32Le(data + cursor + 4);
    leg.alloc_account = ReadU32Le(data + cursor + 8);
    leg.qty_milli = ReadU32Le(data + cursor + 12);
    leg.ref_offset = ReadU32Le(data + cursor + 16);
    leg.ref_len = ReadU32Le(data + cursor + 20);
    leg.flags = ReadU32Le(data + cursor + 24);
    out->push_back(leg);
    cursor += kSessionLegSize;
  }

  return Status::kOk;
}

Status SessionWireCodec::ReadRefBlob(const std::uint8_t* data, std::size_t size,
                                     std::size_t offset,
                                     std::vector<std::uint8_t>* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  if (offset >= size) {
    out->clear();
    return Status::kOk;
  }
  out->assign(data + offset, data + size);
  return Status::kOk;
}

SessionEncodeResult SessionWireCodec::EncodeSession(
    const WireSessionHeader& header, const std::vector<WireSessionLeg>& legs,
    const std::uint8_t* ref_data, std::size_t ref_size) {
  SessionEncodeResult result{};
  result.status = ValidateHeader(header);
  if (result.status != Status::kOk) {
    return result;
  }

  if (legs.size() != header.leg_count) {
    result.status = Status::kBoundsError;
    return result;
  }

  std::vector<std::uint8_t> wire;
  wire.reserve(kSessionHeaderSize + legs.size() * kSessionLegSize + ref_size);

  WireSessionHeader hdr = header;
  hdr.merge_digest = ComputeLegTableDigest(legs);
  if (ref_data != nullptr && ref_size > 0) {
    hdr.merge_digest ^= ComputeRefDigest(ref_data, ref_size);
  }

  AppendU32Le(&wire, hdr.magic);
  AppendU16Le(&wire, hdr.version);
  AppendU16Le(&wire, hdr.leg_count);
  AppendU32Le(&wire, hdr.session_id);
  AppendU32Le(&wire, hdr.checkpoint_seq);
  AppendU32Le(&wire, hdr.flags);
  AppendU32Le(&wire, hdr.merge_digest);

  for (const WireSessionLeg& leg : legs) {
    AppendLegBytes(&wire, leg);
  }

  if (ref_data != nullptr && ref_size > 0) {
    wire.insert(wire.end(), ref_data, ref_data + ref_size);
  }

  result.wire_bytes = std::move(wire);
  result.merge_digest = hdr.merge_digest;
  result.ref_blob_size = ref_size;
  result.status = Status::kOk;
  return result;
}

SessionDecodeResult SessionWireCodec::DecodeSession(const std::uint8_t* data,
                                                    std::size_t size) {
  SessionDecodeResult result{};
  std::size_t consumed = 0;

  result.status = ReadHeader(data, size, &result.frame.header, &consumed);
  if (result.status != Status::kOk) {
    return result;
  }

  result.status = ValidateHeader(result.frame.header);
  if (result.status != Status::kOk) {
    return result;
  }

  result.status = ReadLegs(data, size, consumed, result.frame.header.leg_count,
                             &result.frame.legs);
  if (result.status != Status::kOk) {
    return result;
  }

  consumed += static_cast<std::size_t>(result.frame.header.leg_count) *
              kSessionLegSize;

  result.status = ReadRefBlob(data, size, consumed, &result.frame.ref_blob);
  if (result.status != Status::kOk) {
    return result;
  }

  if (config_.validate_ref_bounds) {
    result.status = ValidateLegRefs(result.frame);
    if (result.status != Status::kOk) {
      return result;
    }
  }

  if (config_.enable_merge_staging) {
    result.status = StageMergeSlots(&result.frame);
    if (result.status != Status::kOk) {
      return result;
    }
  }

  result.consumed_bytes = size;
  return result;
}

Status SessionWireCodec::StageMergeSlots(SessionWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }

  frame->merge_slots.clear();
  merge_table_.clear();
  next_slot_id_ = 1;

  for (const WireSessionLeg& leg : frame->legs) {
    if (leg.ref_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(leg.ref_offset, leg.ref_len,
                             frame->ref_blob.size())) {
      return Status::kBoundsError;
    }

    MergeSlot slot{};
    slot.slot_id = next_slot_id_++;
    slot.leg_id = leg.leg_id;
    slot.ref_ptr = frame->ref_blob.data() + leg.ref_offset;
    slot.ref_len = leg.ref_len;
    slot.checkpoint_seq = frame->header.checkpoint_seq;
    slot.pinned = (leg.flags & kLegFlagCheckpointPin) != 0;

    frame->merge_slots.push_back(slot);
    merge_table_.push_back(slot);

    if (merge_table_.size() > kMaxDeferredSlots) {
      return Status::kBoundsError;
    }
  }

  return Status::kOk;
}

Status SessionWireCodec::ClearMergeSlots(SessionWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }
  for (MergeSlot& slot : frame->merge_slots) {
    slot.ref_ptr = nullptr;
    slot.pinned = false;
  }
  for (MergeSlot& slot : merge_table_) {
    slot.ref_ptr = nullptr;
    slot.pinned = false;
  }
  frame->merge_slots.clear();
  merge_table_.clear();
  return Status::kOk;
}

SessionEncodeResult SessionWireCodec::EncodeLegsOnly(
    const std::vector<WireSessionLeg>& legs,
    const std::vector<std::uint8_t>& ref_blob) {
  WireSessionHeader header{};
  header.magic = kSessionMagic;
  header.version = kWireVersion;
  header.leg_count = static_cast<std::uint16_t>(legs.size());
  header.session_id = 1;
  header.checkpoint_seq = 1;
  header.flags = kLegFlagMergePending;
  return EncodeSession(header, legs, ref_blob.data(), ref_blob.size());
}

Status SessionWireCodec::PatchLegRefOffset(WireSessionLeg* leg,
                                           std::uint32_t new_offset) {
  if (leg == nullptr) {
    return Status::kBoundsError;
  }
  leg->ref_offset = new_offset;
  return Status::kOk;
}

Status SessionWireCodec::RelocateRefBlob(SessionWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }

  std::vector<std::uint8_t> compact;
  compact.reserve(frame->ref_blob.size());

  for (WireSessionLeg& leg : frame->legs) {
    if (leg.ref_len == 0) {
      leg.ref_offset = static_cast<std::uint32_t>(compact.size());
      continue;
    }
    if (!util::SliceInBounds(leg.ref_offset, leg.ref_len,
                             frame->ref_blob.size())) {
      return Status::kBoundsError;
    }
    const std::uint8_t* src = frame->ref_blob.data() + leg.ref_offset;
    leg.ref_offset = static_cast<std::uint32_t>(compact.size());
    compact.insert(compact.end(), src, src + leg.ref_len);
  }

  frame->ref_blob.swap(compact);
  return Status::kOk;
}

SessionDecodeResult SessionWireCodec::DecodePartial(const std::uint8_t* data,
                                                    std::size_t size) {
  SessionDecodeResult result{};
  if (size < kSessionHeaderSize + kSessionLegSize) {
    result.status = Status::kPartialFrame;
    return result;
  }
  return DecodeSession(data, size);
}

std::uint32_t SessionWireCodec::EstimateEncodedSize(
    std::uint16_t leg_count, std::size_t ref_size) {
  return static_cast<std::uint32_t>(kSessionHeaderSize +
                                    static_cast<std::size_t>(leg_count) *
                                        kSessionLegSize +
                                    ref_size);
}

}  // namespace wire
}  // namespace tkr
