#include "tkr/engine/merge_deferred_ledger.h"

#include "tkr/util/bounds.h"

namespace tkr {
namespace engine {
namespace {

constexpr std::uint32_t kCrossFrameMetaLegId = 0;

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

MergeDeferredLedger& MergeDeferredLedger::Global() {
  static MergeDeferredLedger ledger;
  return ledger;
}

void MergeDeferredLedger::Reset() {
  pending_session_ = 0;
  pending_checkpoint_ = 0;
  ref_arena_.clear();
  pending_slots_.clear();
  pending_active_ = false;
  mutation_applied_ = false;
}

bool MergeDeferredLedger::HasPending(std::uint32_t session_id) const {
  return pending_active_ && pending_session_ == session_id;
}

bool MergeDeferredLedger::ReadyForSuccessor(std::uint32_t session_id) const {
  return pending_active_ && mutation_applied_ && pending_session_ == session_id;
}

Status MergeDeferredLedger::StageOpeningSession(const SessionWireFrame& frame,
                                                std::uint32_t checkpoint_seq) {
  if (checkpoint_seq != 1 || frame.ref_blob.empty()) {
    return Status::kSessionGap;
  }

  std::vector<MergeSlot> slots;
  Status slot_st = BuildCrossFrameMergeSlots(frame, frame.ref_blob, &slots);
  if (slot_st != Status::kOk) {
    return slot_st;
  }

  pending_session_ = frame.header.session_id;
  pending_checkpoint_ = checkpoint_seq;
  ref_arena_ = frame.ref_blob;
  pending_slots_.clear();
  mutation_applied_ = false;

  for (const MergeSlot& slot : slots) {
    if (!slot.pinned || slot.ref_len == 0 || slot.ref_ptr == nullptr) {
      continue;
    }
    if (slot.ref_ptr < frame.ref_blob.data() ||
        slot.ref_ptr + slot.ref_len >
            frame.ref_blob.data() + frame.ref_blob.size()) {
      return Status::kBoundsError;
    }

    MergeSlot pinned = slot;
    const std::size_t offset =
        static_cast<std::size_t>(slot.ref_ptr - frame.ref_blob.data());
    pinned.ref_ptr = ref_arena_.data() + offset;
    pending_slots_.push_back(pinned);
  }

  if (pending_slots_.empty()) {
    return Status::kBoundsError;
  }

  pending_active_ = true;
  return Status::kOk;
}

Status MergeDeferredLedger::ApplyMutation(std::uint32_t session_id) {
  if (!pending_active_ || pending_session_ != session_id) {
    return Status::kSessionGap;
  }
  mutation_applied_ = true;
  return Status::kOk;
}

Status MergeDeferredLedger::ArmSuccessorFlush(MergeDigestEngine* digest) {
  if (!ReadyForSuccessor(pending_session_) || digest == nullptr) {
    return Status::kSessionGap;
  }

  std::vector<MergeSlot> stale_slots = pending_slots_;
  ref_arena_.clear();
  ref_arena_.shrink_to_fit();

  pending_active_ = false;
  mutation_applied_ = false;
  pending_slots_.clear();
  pending_checkpoint_ = 0;

  digest->RegisterMergeSlots(stale_slots);
  return Status::kOk;
}

std::uint32_t ExtractSessionCrossFrameSequence(const SessionWireFrame& frame) {
  for (const WireSessionLeg& leg : frame.legs) {
    if (leg.leg_id == kCrossFrameMetaLegId) {
      return leg.alloc_account;
    }
  }
  return frame.header.checkpoint_seq;
}

Status BuildCrossFrameMergeSlots(const SessionWireFrame& frame,
                                 const std::vector<std::uint8_t>& ref_blob,
                                 std::vector<MergeSlot>* slots) {
  if (slots == nullptr) {
    return Status::kBoundsError;
  }
  slots->clear();
  std::uint32_t slot_id = 1;
  for (const WireSessionLeg& leg : frame.legs) {
    if (leg.leg_id == kCrossFrameMetaLegId || leg.ref_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(leg.ref_offset, leg.ref_len, ref_blob.size())) {
      return Status::kBoundsError;
    }
    MergeSlot slot{};
    slot.slot_id = slot_id++;
    slot.leg_id = leg.leg_id;
    slot.ref_ptr = ref_blob.data() + leg.ref_offset;
    slot.ref_len = leg.ref_len;
    slot.checkpoint_seq = frame.header.checkpoint_seq;
    slot.pinned = true;
    slots->push_back(slot);
  }
  if (slots->empty()) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status DecodeSessionWireFrame(const std::uint8_t* data, std::size_t size,
                              SessionWireFrame* frame,
                              std::size_t* consumed_bytes) {
  if (frame == nullptr || consumed_bytes == nullptr || data == nullptr ||
      size < 24) {
    return Status::kTruncated;
  }

  frame->header.magic = ReadU32Le(data + 0);
  frame->header.version = ReadU16Le(data + 4);
  frame->header.leg_count = ReadU16Le(data + 6);
  frame->header.session_id = ReadU32Le(data + 8);
  frame->header.checkpoint_seq = ReadU32Le(data + 12);
  frame->header.flags = ReadU32Le(data + 16);
  frame->header.merge_digest = ReadU32Le(data + 20);

  if (frame->header.magic != kSessionMagic) {
    return Status::kInvalidMagic;
  }
  if (frame->header.leg_count > kMaxSessionLegs) {
    return Status::kBoundsError;
  }

  const std::size_t table_bytes =
      static_cast<std::size_t>(frame->header.leg_count) * 28;
  if (!util::SectionBodyInBounds(24, table_bytes, size)) {
    return Status::kBoundsError;
  }

  frame->legs.clear();
  frame->legs.reserve(frame->header.leg_count);
  std::size_t cursor = 24;
  for (std::uint16_t i = 0; i < frame->header.leg_count; ++i) {
    WireSessionLeg leg{};
    leg.leg_id = ReadU32Le(data + cursor + 0);
    leg.cl_ord_id = ReadU32Le(data + cursor + 4);
    leg.alloc_account = ReadU32Le(data + cursor + 8);
    leg.qty_milli = ReadU32Le(data + cursor + 12);
    leg.ref_offset = ReadU32Le(data + cursor + 16);
    leg.ref_len = ReadU32Le(data + cursor + 20);
    leg.flags = ReadU32Le(data + cursor + 24);
    frame->legs.push_back(leg);
    cursor += 28;
  }

  std::size_t ref_span = 0;
  for (const WireSessionLeg& leg : frame->legs) {
    if (leg.ref_len == 0) {
      continue;
    }
    const std::size_t end =
        static_cast<std::size_t>(leg.ref_offset + leg.ref_len);
    if (end > ref_span) {
      ref_span = end;
    }
  }

  if (!util::SectionBodyInBounds(cursor, ref_span, size)) {
    return Status::kBoundsError;
  }

  frame->ref_blob.assign(data + cursor, data + cursor + ref_span);
  frame->merge_slots.clear();
  *consumed_bytes = cursor + ref_span;
  return Status::kOk;
}

}  // namespace engine
}  // namespace tkr
