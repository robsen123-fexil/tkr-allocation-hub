#include "tkr/engine/session_merger.h"

#include "tkr/util/bounds.h"

#include <cstring>

namespace tkr {
namespace engine {
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

SessionMerger::SessionMerger(SessionMergerConfig config) : config_(config) {}

Status SessionMerger::ReadSessionHeader(const std::uint8_t* data, std::size_t size,
                                        SessionWireFrame* out) {
  if (out == nullptr || data == nullptr || size < 24) {
    return Status::kTruncated;
  }
  out->header.magic = ReadU32Le(data + 0);
  out->header.version = ReadU16Le(data + 4);
  out->header.leg_count = ReadU16Le(data + 6);
  out->header.session_id = ReadU32Le(data + 8);
  out->header.checkpoint_seq = ReadU32Le(data + 12);
  out->header.flags = ReadU32Le(data + 16);
  out->header.merge_digest = ReadU32Le(data + 20);

  if (out->header.magic != kSessionMagic) {
    return Status::kInvalidMagic;
  }
  if (out->header.leg_count > config_.max_legs ||
      out->header.leg_count > kMaxSessionLegs) {
    return Status::kBoundsError;
  }

  const std::size_t table_bytes =
      static_cast<std::size_t>(out->header.leg_count) * 28;
  if (!util::SectionBodyInBounds(28, table_bytes, size)) {
    return Status::kBoundsError;
  }

  out->legs.clear();
  out->legs.reserve(out->header.leg_count);
  std::size_t cursor = 24;
  for (std::uint16_t i = 0; i < out->header.leg_count; ++i) {
    WireSessionLeg leg{};
    leg.leg_id = ReadU32Le(data + cursor + 0);
    leg.cl_ord_id = ReadU32Le(data + cursor + 4);
    leg.alloc_account = ReadU32Le(data + cursor + 8);
    leg.qty_milli = ReadU32Le(data + cursor + 12);
    leg.ref_offset = ReadU32Le(data + cursor + 16);
    leg.ref_len = ReadU32Le(data + cursor + 20);
    leg.flags = ReadU32Le(data + cursor + 24);
    out->legs.push_back(leg);
    cursor += 28;
  }

  if (cursor < size) {
    out->ref_blob.assign(data + cursor, data + size);
  } else {
    out->ref_blob.clear();
  }
  return Status::kOk;
}

Status SessionMerger::PinMergeSlots(SessionWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }
  frame->merge_slots.clear();
  std::uint32_t slot_id = 1;
  for (const WireSessionLeg& leg : frame->legs) {
    if (leg.ref_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(leg.ref_offset, leg.ref_len, frame->ref_blob.size())) {
      return Status::kBoundsError;
    }
    MergeSlot slot{};
    slot.slot_id = slot_id++;
    slot.leg_id = leg.leg_id;
    slot.ref_ptr = frame->ref_blob.data() + leg.ref_offset;
    slot.ref_len = leg.ref_len;
    slot.checkpoint_seq = frame->header.checkpoint_seq;
    slot.pinned = true;
    frame->merge_slots.push_back(slot);
  }
  return Status::kOk;
}

SessionMergeResult SessionMerger::DecodeSession(const std::uint8_t* data,
                                                std::size_t len) {
  SessionMergeResult result{};
  result.status = ReadSessionHeader(data, len, &result.frame);
  return result;
}

SessionMergeResult SessionMerger::GraftSessionLegs(SessionWireFrame* frame) {
  SessionMergeResult result{};
  result.status = Status::kOk;
  if (frame == nullptr) {
    result.status = Status::kBoundsError;
    return result;
  }

  if (!config_.enable_graft_realloc) {
    result.frame = *frame;
    return result;
  }

  std::vector<std::uint8_t> grafted;
  grafted.reserve(frame->ref_blob.size() + frame->legs.size() * 16);
  for (const WireSessionLeg& leg : frame->legs) {
    if (leg.ref_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(leg.ref_offset, leg.ref_len, frame->ref_blob.size())) {
      result.status = Status::kBoundsError;
      return result;
    }
    const std::uint8_t* src = frame->ref_blob.data() + leg.ref_offset;
    grafted.insert(grafted.end(), src, src + leg.ref_len);
    grafted.push_back(static_cast<std::uint8_t>(leg.leg_id & 0xFFu));
    grafted.push_back(static_cast<std::uint8_t>((leg.leg_id >> 8) & 0xFFu));
    ++result.legs_grafted;
  }

  frame->ref_blob.swap(grafted);
  result.frame = *frame;
  return result;
}

Status SessionMerger::PinLegMergeSlots(SessionWireFrame* frame) {
  return PinMergeSlots(frame);
}

SessionMergeResult SessionMerger::MergeSessionCheckpoint(SessionWireFrame* frame) {
  SessionMergeResult result{};
  result.status = Status::kOk;
  if (frame == nullptr) {
    result.status = Status::kBoundsError;
    return result;
  }

  result.status = PinMergeSlots(frame);
  if (result.status != Status::kOk) {
    return result;
  }
  result.slots_registered =
      static_cast<std::uint32_t>(frame->merge_slots.size());

  SessionMergeResult graft = GraftSessionLegs(frame);
  if (graft.status != Status::kOk) {
    result.status = graft.status;
    return result;
  }
  result.legs_grafted = graft.legs_grafted;
  result.frame = *frame;
  return result;
}

}  // namespace engine
}  // namespace tkr
