#include "tkr/ledger/audit_spool.h"

#include "tkr/util/bounds.h"

#include <algorithm>

namespace tkr {
namespace ledger {
namespace {

constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

struct AuditChainLink {
  std::uint32_t sequence;
  std::uint32_t digest;
  std::uint64_t timestamp_ns;
};

std::uint32_t MixDigest(std::uint32_t hash, std::uint32_t value) {
  hash ^= value;
  hash *= kFnvPrime;
  return hash;
}

}  // namespace

AuditSpool& GlobalAuditSpool() {
  static AuditSpool spool(AuditSpoolConfig{8192, true});
  return spool;
}

AuditSpool::AuditSpool(AuditSpoolConfig config)
    : config_(config),
      next_sequence_(1),
      chain_digest_(kFnvOffset),
      timestamp_counter_(0) {}

std::uint64_t AuditSpool::NextTimestamp() {
  return ++timestamp_counter_;
}

std::uint32_t AuditSpool::ComputeEventDigest(const AuditEvent& event) const {
  std::uint8_t buf[32];
  buf[0] = static_cast<std::uint8_t>(event.kind);
  buf[1] = static_cast<std::uint8_t>((event.sequence >> 24) & 0xFF);
  buf[2] = static_cast<std::uint8_t>((event.sequence >> 16) & 0xFF);
  buf[3] = static_cast<std::uint8_t>((event.sequence >> 8) & 0xFF);
  buf[4] = static_cast<std::uint8_t>(event.sequence & 0xFF);
  buf[5] = static_cast<std::uint8_t>((event.desk_id >> 24) & 0xFF);
  buf[6] = static_cast<std::uint8_t>((event.desk_id >> 16) & 0xFF);
  buf[7] = static_cast<std::uint8_t>((event.desk_id >> 8) & 0xFF);
  buf[8] = static_cast<std::uint8_t>(event.desk_id & 0xFF);
  buf[9] = static_cast<std::uint8_t>((event.batch_id >> 24) & 0xFF);
  buf[10] = static_cast<std::uint8_t>((event.batch_id >> 16) & 0xFF);
  buf[11] = static_cast<std::uint8_t>((event.batch_id >> 8) & 0xFF);
  buf[12] = static_cast<std::uint8_t>(event.batch_id & 0xFF);
  buf[13] = static_cast<std::uint8_t>((event.account_id >> 24) & 0xFF);
  buf[14] = static_cast<std::uint8_t>((event.account_id >> 16) & 0xFF);
  buf[15] = static_cast<std::uint8_t>((event.account_id >> 8) & 0xFF);
  buf[16] = static_cast<std::uint8_t>(event.account_id & 0xFF);

  std::uint32_t digest = util::Fnv1a32(buf, 17);
  digest = MixDigest(digest, util::Fnv1a32(event.detail));
  digest = MixDigest(digest, event.payload_digest);
  return digest;
}

void AuditSpool::EvictOldestIfNeeded() {
  if (events_.size() < config_.max_events) {
    return;
  }
  const std::size_t evict_count = events_.size() - config_.max_events + 1;
  events_.erase(events_.begin(),
                events_.begin() + static_cast<std::ptrdiff_t>(evict_count));
}

AuditSpoolResult AuditSpool::Append(AuditEventKind kind, std::uint32_t desk_id,
                                    std::uint32_t batch_id,
                                    std::uint32_t account_id,
                                    const std::string& detail) {
  AuditSpoolResult result{};
  EvictOldestIfNeeded();

  AuditEvent event{};
  event.kind = kind;
  event.sequence = next_sequence_++;
  event.desk_id = desk_id;
  event.batch_id = batch_id;
  event.account_id = account_id;
  event.timestamp_ns = NextTimestamp();
  event.detail = detail;
  event.payload_digest = 0;

  if (config_.compute_digests) {
    event.payload_digest = ComputeEventDigest(event);
    chain_digest_ = MixDigest(chain_digest_, event.payload_digest);
    chain_digest_ = MixDigest(chain_digest_, event.sequence);
  }

  events_.push_back(event);
  result.events_written = 1;
  result.chain_digest = chain_digest_;
  result.status = Status::kOk;
  return result;
}

AuditSpoolResult AuditSpool::AppendBatchSummary(
    const AllocationBatchSummary& summary) {
  std::string detail;
  detail.reserve(128);
  detail += "records=";
  detail += std::to_string(summary.record_count);
  detail += " qty=";
  detail += std::to_string(summary.total_qty_milli);
  detail += " flags=";
  detail += std::to_string(summary.flags);
  detail += " margin=";
  detail += summary.margin_cleared ? "ok" : "fail";
  detail += " compliance=";
  detail += summary.compliance_cleared ? "ok" : "fail";

  AuditEventKind kind = AuditEventKind::kBatchProcessed;
  if (!summary.compliance_cleared) {
    kind = AuditEventKind::kComplianceReject;
  } else if (!summary.margin_cleared) {
    kind = AuditEventKind::kMarginBreach;
  }

  return Append(kind, summary.desk_id, summary.batch_id, 0, detail);
}

AuditSpoolResult AuditSpool::Flush() {
  AuditSpoolResult result{};
  result.events_written = static_cast<std::uint32_t>(events_.size());
  result.chain_digest = chain_digest_;
  result.status = Status::kOk;

  std::vector<AuditChainLink> chain;
  chain.reserve(events_.size());
  for (const AuditEvent& event : events_) {
    AuditChainLink link{};
    link.sequence = event.sequence;
    link.digest = event.payload_digest;
    link.timestamp_ns = event.timestamp_ns;
    chain.push_back(link);
  }

  std::sort(chain.begin(), chain.end(),
            [](const AuditChainLink& a, const AuditChainLink& b) {
              return a.sequence < b.sequence;
            });

  std::uint32_t verify = kFnvOffset;
  for (const AuditChainLink& link : chain) {
    verify = MixDigest(verify, link.digest);
    verify = MixDigest(verify, link.sequence);
  }

  if (verify != chain_digest_ && !events_.empty()) {
    result.events_dropped = 1;
  }

  return result;
}

void AuditSpool::Reset() {
  events_.clear();
  next_sequence_ = 1;
  chain_digest_ = kFnvOffset;
  timestamp_counter_ = 0;
}

}  // namespace ledger
}  // namespace tkr
