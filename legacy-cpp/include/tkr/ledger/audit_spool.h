#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tkr {
namespace ledger {

enum class AuditEventKind : std::uint8_t {
  kUnknown = 0,
  kBatchProcessed,
  kAllocationApplied,
  kComplianceReject,
  kMarginBreach,
  kCheckpointSealed,
  kEnvelopeSealed,
  kSessionMerged,
  kRestrictionBlock,
  kFeeAccrued,
};

struct AuditEvent {
  AuditEventKind kind;
  std::uint32_t sequence;
  std::uint32_t desk_id;
  std::uint32_t batch_id;
  std::uint32_t account_id;
  std::uint32_t payload_digest;
  std::uint64_t timestamp_ns;
  std::string detail;
};

struct AuditSpoolConfig {
  std::uint32_t max_events;
  bool compute_digests;
};

struct AuditSpoolResult {
  Status status;
  std::uint32_t events_written;
  std::uint32_t events_dropped;
  std::uint32_t chain_digest;
};

class AuditSpool {
 public:
  explicit AuditSpool(AuditSpoolConfig config);

  AuditSpoolResult Append(AuditEventKind kind, std::uint32_t desk_id,
                          std::uint32_t batch_id, std::uint32_t account_id,
                          const std::string& detail);
  AuditSpoolResult AppendBatchSummary(const AllocationBatchSummary& summary);
  AuditSpoolResult Flush();

  const std::vector<AuditEvent>& Events() const { return events_; }
  std::uint32_t ChainDigest() const { return chain_digest_; }
  void Reset();

 private:
  std::uint32_t ComputeEventDigest(const AuditEvent& event) const;
  void EvictOldestIfNeeded();
  std::uint64_t NextTimestamp();

  AuditSpoolConfig config_;
  std::vector<AuditEvent> events_;
  std::uint32_t next_sequence_;
  std::uint32_t chain_digest_;
  std::uint64_t timestamp_counter_;
};

AuditSpool& GlobalAuditSpool();

}  // namespace ledger
}  // namespace tkr
