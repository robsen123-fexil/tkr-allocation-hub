#include "tkr/ledger/checkpoint_ledger.h"

#include "tkr/util/bounds.h"

#include <algorithm>

namespace tkr {
namespace ledger {
namespace {

constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

std::uint32_t MixField(std::uint32_t hash, std::uint32_t value) {
  hash ^= value;
  hash *= kFnvPrime;
  return hash;
}

}  // namespace

CheckpointLedger& GlobalCheckpointLedger() {
  static CheckpointLedger ledger(CheckpointLedgerConfig{1024, true});
  return ledger;
}

CheckpointLedger::CheckpointLedger(CheckpointLedgerConfig config)
    : config_(config), latest_seq_(0) {}

std::uint32_t CheckpointLedger::ComputeMergeDigest(
    const SessionWireFrame& frame) const {
  std::uint32_t digest = kFnvOffset;

  digest = MixField(digest, frame.header.session_id);
  digest = MixField(digest, frame.header.leg_count);
  digest = MixField(digest, frame.header.checkpoint_seq);
  digest = MixField(digest, frame.header.flags);

  for (const WireSessionLeg& leg : frame.legs) {
    digest = MixField(digest, leg.leg_id);
    digest = MixField(digest, leg.cl_ord_id);
    digest = MixField(digest, leg.alloc_account);
    digest = MixField(digest, leg.qty_milli);
    digest = MixField(digest, leg.flags);

    if (leg.ref_len > 0 && leg.ref_offset < frame.ref_blob.size()) {
      const std::size_t avail = frame.ref_blob.size() - leg.ref_offset;
      const std::size_t len =
          (leg.ref_len <= avail) ? leg.ref_len : avail;
      digest = MixField(digest,
                        util::Fnv1a32(frame.ref_blob.data() + leg.ref_offset,
                                      len));
    }
  }

  for (const MergeSlot& slot : frame.merge_slots) {
    if (!slot.pinned) {
      continue;
    }
    digest = MixField(digest, slot.slot_id);
    digest = MixField(digest, slot.leg_id);
    digest = MixField(digest, slot.checkpoint_seq);
    if (slot.ref_ptr != nullptr && slot.ref_len > 0) {
      digest = MixField(digest, util::Fnv1a32(slot.ref_ptr, slot.ref_len));
    }
  }

  return digest;
}

void CheckpointLedger::EvictOldestIfNeeded() {
  if (records_.size() < config_.max_checkpoints) {
    return;
  }
  records_.erase(records_.begin());
}

bool CheckpointLedger::ValidateSequenceChain() const {
  if (records_.empty()) {
    return true;
  }

  std::uint32_t expected_prev = 0;
  for (const CheckpointRecord& rec : records_) {
    if (rec.checkpoint_seq != expected_prev + 1) {
      return false;
    }
    if (rec.prev_checkpoint_seq != expected_prev) {
      return false;
    }
    if (!rec.valid) {
      return false;
    }
    expected_prev = rec.checkpoint_seq;
  }
  return true;
}

CheckpointSealResult CheckpointLedger::SealSession(
    const SessionWireFrame& frame) {
  CheckpointSealResult result{};
  EvictOldestIfNeeded();

  CheckpointRecord record{};
  record.checkpoint_seq = latest_seq_ + 1;
  record.session_id = frame.header.session_id;
  record.leg_count = frame.header.leg_count;
  record.merge_digest = ComputeMergeDigest(frame);
  record.prev_checkpoint_seq = latest_seq_;
  record.sealed_at_ns =
      static_cast<std::uint64_t>(record.checkpoint_seq) * 1000000u;
  record.valid = true;

  if (config_.verify_chain && latest_seq_ > 0) {
    const CheckpointRecord prev = Lookup(latest_seq_);
    if (prev.session_id != 0 && prev.session_id != record.session_id) {
      record.valid = false;
    }
  }

  records_.push_back(record);
  latest_seq_ = record.checkpoint_seq;

  result.checkpoint_seq = record.checkpoint_seq;
  result.merge_digest = record.merge_digest;
  result.chain_valid = ValidateSequenceChain();
  result.status = result.chain_valid ? Status::kOk : Status::kSessionGap;
  return result;
}

CheckpointVerifyResult CheckpointLedger::VerifyChain() const {
  CheckpointVerifyResult result{};
  result.checkpoints_verified =
      static_cast<std::uint32_t>(records_.size());
  result.chain_intact = ValidateSequenceChain();
  result.status = result.chain_intact ? Status::kOk : Status::kSessionGap;

  if (!result.chain_intact) {
    std::uint32_t expected = 0;
    for (const CheckpointRecord& rec : records_) {
      if (rec.checkpoint_seq != expected + 1) {
        result.first_gap_seq = expected + 1;
        break;
      }
      expected = rec.checkpoint_seq;
    }
  }

  return result;
}

CheckpointRecord CheckpointLedger::Lookup(std::uint32_t checkpoint_seq) const {
  for (const CheckpointRecord& rec : records_) {
    if (rec.checkpoint_seq == checkpoint_seq) {
      return rec;
    }
  }
  return CheckpointRecord{};
}

void CheckpointLedger::Reset() {
  records_.clear();
  latest_seq_ = 0;
}

}  // namespace ledger
}  // namespace tkr
