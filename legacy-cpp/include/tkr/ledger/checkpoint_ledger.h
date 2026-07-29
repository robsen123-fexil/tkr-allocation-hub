#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace ledger {

struct CheckpointRecord {
  std::uint32_t checkpoint_seq;
  std::uint32_t session_id;
  std::uint32_t leg_count;
  std::uint32_t merge_digest;
  std::uint32_t prev_checkpoint_seq;
  std::uint64_t sealed_at_ns;
  bool valid;
};

struct CheckpointLedgerConfig {
  std::uint32_t max_checkpoints;
  bool verify_chain;
};

struct CheckpointSealResult {
  Status status;
  std::uint32_t checkpoint_seq;
  std::uint32_t merge_digest;
  bool chain_valid;
};

struct CheckpointVerifyResult {
  Status status;
  std::uint32_t checkpoints_verified;
  std::uint32_t first_gap_seq;
  bool chain_intact;
};

class CheckpointLedger {
 public:
  explicit CheckpointLedger(CheckpointLedgerConfig config);

  CheckpointSealResult SealSession(const SessionWireFrame& frame);
  CheckpointVerifyResult VerifyChain() const;
  CheckpointRecord Lookup(std::uint32_t checkpoint_seq) const;

  const std::vector<CheckpointRecord>& Records() const { return records_; }
  std::uint32_t LatestSeq() const { return latest_seq_; }
  void Reset();

 private:
  std::uint32_t ComputeMergeDigest(const SessionWireFrame& frame) const;
  bool ValidateSequenceChain() const;
  void EvictOldestIfNeeded();

  CheckpointLedgerConfig config_;
  std::vector<CheckpointRecord> records_;
  std::uint32_t latest_seq_;
};

CheckpointLedger& GlobalCheckpointLedger();

}  // namespace ledger
}  // namespace tkr
