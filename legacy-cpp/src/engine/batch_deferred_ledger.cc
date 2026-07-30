#include "tkr/engine/batch_deferred_ledger.h"

#include "tkr/util/bounds.h"

namespace tkr {
namespace engine {
namespace {

constexpr std::uint32_t kCrossFrameMetaRecordId = 0;

}  // namespace

BatchDeferredLedger& BatchDeferredLedger::Global() {
  static BatchDeferredLedger ledger;
  return ledger;
}

void BatchDeferredLedger::Reset() {
  pending_session_ = 0;
  pending_trade_date_ = 0;
  pending_sequence_ = 0;
  payload_arena_.clear();
  pending_slots_.clear();
  pending_active_ = false;
  mutation_applied_ = false;
  mutation_rounds_ = 0;
}

bool BatchDeferredLedger::HasPending(std::uint32_t session_id,
                                     std::uint32_t trade_date_yyyymmdd) const {
  return pending_active_ && pending_session_ == session_id &&
         pending_trade_date_ == trade_date_yyyymmdd && pending_sequence_ == 1;
}

bool BatchDeferredLedger::ReadyForSuccessor(
    std::uint32_t session_id, std::uint32_t trade_date_yyyymmdd) const {
  return pending_active_ && mutation_applied_ && mutation_rounds_ >= 2 &&
         pending_session_ == session_id &&
         pending_trade_date_ == trade_date_yyyymmdd && pending_sequence_ == 1;
}

Status BatchDeferredLedger::ApplyMutation(std::uint32_t session_id,
                                          std::uint32_t trade_date_yyyymmdd) {
  if (!pending_active_ || pending_session_ != session_id ||
      pending_trade_date_ != trade_date_yyyymmdd) {
    return Status::kSessionGap;
  }
  mutation_applied_ = true;
  ++mutation_rounds_;
  return Status::kOk;
}

Status BatchDeferredLedger::StageOpeningFrame(const BatchWireFrame& frame,
                                              std::uint32_t sequence) {
  if (sequence != 1) {
    return Status::kSessionGap;
  }
  if (frame.payload_blob.empty()) {
    return Status::kBoundsError;
  }

  pending_session_ = frame.header.desk_id;
  pending_trade_date_ = frame.header.trade_date_yyyymmdd;
  pending_sequence_ = sequence;
  payload_arena_ = frame.payload_blob;
  pending_slots_.clear();

  for (const DeferredSlot& slot : frame.deferred_slots) {
    if (!slot.active || slot.payload_len == 0 || slot.payload_ptr == nullptr) {
      continue;
    }
    if (slot.payload_ptr < frame.payload_blob.data() ||
        slot.payload_ptr + slot.payload_len >
            frame.payload_blob.data() + frame.payload_blob.size()) {
      return Status::kBoundsError;
    }

    DeferredSlot pinned = slot;
    const std::size_t offset =
        static_cast<std::size_t>(slot.payload_ptr - frame.payload_blob.data());
    pinned.payload_ptr = payload_arena_.data() + offset;
    pending_slots_.push_back(pinned);
  }

  if (pending_slots_.empty()) {
    return Status::kBoundsError;
  }

  pending_active_ = true;
  return Status::kOk;
}

Status BatchDeferredLedger::ArmSuccessorFlush(BatchDigestEngine* digest) {
  if (!ReadyForSuccessor(pending_session_, pending_trade_date_) ||
      digest == nullptr) {
    return Status::kSessionGap;
  }

  std::vector<DeferredSlot> stale_slots = pending_slots_;
  payload_arena_.clear();
  payload_arena_.shrink_to_fit();

  pending_active_ = false;
  pending_slots_.clear();
  pending_sequence_ = 0;
  mutation_applied_ = false;
  mutation_rounds_ = 0;

  digest->RegisterDeferredSlots(stale_slots);
  return Status::kOk;
}

std::uint32_t ExtractCrossFrameSequence(const BatchWireFrame& frame) {
  for (const WireBatchRecord& rec : frame.records) {
    if (rec.record_id == kCrossFrameMetaRecordId) {
      return rec.account_id;
    }
  }
  return 0;
}

}  // namespace engine
}  // namespace tkr
