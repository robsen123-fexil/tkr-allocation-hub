#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tkr {
namespace ledger {

enum class JournalEntryKind : std::uint8_t {
  kUnknown = 0,
  kAllocationDebit,
  kAllocationCredit,
  kMarginReserve,
  kMarginRelease,
  kFeeAccrual,
  kSettlementPending,
  kSettlementConfirmed,
  kReconcileAdjustment,
};

enum class ReconcileState : std::uint8_t {
  kPending = 0,
  kMatched,
  kUnmatched,
  kAdjusted,
  kWrittenOff,
};

struct JournalLine {
  JournalEntryKind kind;
  std::uint32_t line_id;
  std::uint32_t batch_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t qty_milli;
  std::int64_t amount_cents;
  std::uint32_t trade_date_yyyymmdd;
  std::uint32_t settle_date_yyyymmdd;
  ReconcileState state;
  std::string reference;
};

struct ReconcileMatch {
  std::uint32_t internal_line_id;
  std::uint32_t external_line_id;
  std::int64_t qty_delta;
  std::int64_t amount_delta;
  ReconcileState resulting_state;
};

struct ReconcileJournalConfig {
  std::uint32_t max_entries;
  bool auto_match_exact;
  std::int64_t qty_tolerance_milli;
  std::int64_t amount_tolerance_cents;
};

struct ReconcileJournalResult {
  Status status;
  std::vector<JournalLine> entries;
  std::vector<ReconcileMatch> matches;
  std::uint32_t matched_count;
  std::uint32_t unmatched_count;
  std::int64_t net_qty_milli;
  std::int64_t net_amount_cents;
};

class ReconcileJournal {
 public:
  explicit ReconcileJournal(ReconcileJournalConfig config);

  Status PostLine(const JournalLine& line);
  Status PostBatch(const BatchWireFrame& frame,
                   std::uint32_t settle_date_yyyymmdd);
  ReconcileJournalResult Reconcile();
  ReconcileJournalResult MatchExternal(
      const std::vector<JournalLine>& external_lines);
  ReconcileJournalResult SummarizeByAccount();
  ReconcileJournalResult VerifyBalanced();
  ReconcileJournalResult FilterByBatch(std::uint32_t batch_id) const;
  Status WriteOffUnmatched();
  Status MarkMatched(std::uint32_t line_id);
  std::vector<JournalLine> PendingLines() const;

  const std::vector<JournalLine>& Lines() const { return lines_; }
  JournalLine Lookup(std::uint32_t line_id) const;
  void Reset();

  static JournalLine MakeAllocationDebit(std::uint32_t batch_id,
                                         std::uint32_t account_id,
                                         std::uint32_t symbol_id,
                                         std::int64_t qty_milli,
                                         std::int64_t amount_cents);
  static JournalLine MakeAllocationCredit(std::uint32_t batch_id,
                                          std::uint32_t account_id,
                                          std::uint32_t symbol_id,
                                          std::int64_t qty_milli,
                                          std::int64_t amount_cents);

 private:
  bool LinesMatch(const JournalLine& internal,
                  const JournalLine& external) const;
  ReconcileMatch AttemptMatch(JournalLine* internal,
                              JournalLine* external);
  void EvictOldestIfNeeded();
  std::uint32_t NextLineId();

  ReconcileJournalConfig config_;
  std::vector<JournalLine> lines_;
  std::uint32_t next_line_id_;
};

ReconcileJournal& GlobalReconcileJournal();

}  // namespace ledger
}  // namespace tkr
