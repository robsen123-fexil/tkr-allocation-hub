#include "tkr/ledger/reconcile_journal.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace tkr {
namespace ledger {

ReconcileJournal& GlobalReconcileJournal() {
  static ReconcileJournal journal(ReconcileJournalConfig{8192, true, 0, 0});
  return journal;
}

ReconcileJournal::ReconcileJournal(ReconcileJournalConfig config)
    : config_(config), next_line_id_(1) {}

std::uint32_t ReconcileJournal::NextLineId() { return next_line_id_++; }

void ReconcileJournal::EvictOldestIfNeeded() {
  while (lines_.size() >= config_.max_entries) {
    lines_.erase(lines_.begin());
  }
}

JournalLine ReconcileJournal::MakeAllocationDebit(
    std::uint32_t batch_id, std::uint32_t account_id, std::uint32_t symbol_id,
    std::int64_t qty_milli, std::int64_t amount_cents) {
  JournalLine line{};
  line.kind = JournalEntryKind::kAllocationDebit;
  line.batch_id = batch_id;
  line.account_id = account_id;
  line.symbol_id = symbol_id;
  line.qty_milli = qty_milli;
  line.amount_cents = amount_cents;
  line.state = ReconcileState::kPending;
  return line;
}

JournalLine ReconcileJournal::MakeAllocationCredit(
    std::uint32_t batch_id, std::uint32_t account_id, std::uint32_t symbol_id,
    std::int64_t qty_milli, std::int64_t amount_cents) {
  JournalLine line{};
  line.kind = JournalEntryKind::kAllocationCredit;
  line.batch_id = batch_id;
  line.account_id = account_id;
  line.symbol_id = symbol_id;
  line.qty_milli = -qty_milli;
  line.amount_cents = -amount_cents;
  line.state = ReconcileState::kPending;
  return line;
}

Status ReconcileJournal::PostLine(const JournalLine& line) {
  EvictOldestIfNeeded();
  JournalLine posted = line;
  posted.line_id = NextLineId();
  lines_.push_back(posted);
  return Status::kOk;
}

Status ReconcileJournal::PostBatch(const BatchWireFrame& frame,
                                   std::uint32_t settle_date_yyyymmdd) {
  for (const WireBatchRecord& rec : frame.records) {
    const std::int64_t amount =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;

    JournalLine debit = MakeAllocationDebit(
        frame.header.desk_id, rec.account_id, rec.symbol_id,
        static_cast<std::int64_t>(rec.qty_milli), amount);
    debit.trade_date_yyyymmdd = frame.header.trade_date_yyyymmdd;
    debit.settle_date_yyyymmdd = settle_date_yyyymmdd;
    debit.reference = "batch:" + std::to_string(rec.record_id);
    Status st = PostLine(debit);
    if (st != Status::kOk) {
      return st;
    }

    JournalLine credit = MakeAllocationCredit(
        frame.header.desk_id, rec.account_id, rec.symbol_id,
        static_cast<std::int64_t>(rec.qty_milli), amount);
    credit.trade_date_yyyymmdd = frame.header.trade_date_yyyymmdd;
    credit.settle_date_yyyymmdd = settle_date_yyyymmdd;
    credit.reference = "batch:" + std::to_string(rec.record_id);
    st = PostLine(credit);
    if (st != Status::kOk) {
      return st;
    }

    if ((frame.header.flags & kBatchFlagMarginCheck) != 0) {
      JournalLine margin{};
      margin.kind = JournalEntryKind::kMarginReserve;
      margin.batch_id = frame.header.desk_id;
      margin.account_id = rec.account_id;
      margin.symbol_id = rec.symbol_id;
      margin.qty_milli = 0;
      margin.amount_cents = amount / 10;
      margin.trade_date_yyyymmdd = frame.header.trade_date_yyyymmdd;
      margin.settle_date_yyyymmdd = settle_date_yyyymmdd;
      margin.state = ReconcileState::kPending;
      margin.reference = "margin:" + std::to_string(rec.record_id);
      st = PostLine(margin);
      if (st != Status::kOk) {
        return st;
      }
    }
  }
  return Status::kOk;
}

JournalLine ReconcileJournal::Lookup(std::uint32_t line_id) const {
  for (const JournalLine& line : lines_) {
    if (line.line_id == line_id) {
      return line;
    }
  }
  return JournalLine{};
}

bool ReconcileJournal::LinesMatch(const JournalLine& internal,
                                  const JournalLine& external) const {
  if (internal.account_id != external.account_id) {
    return false;
  }
  if (internal.symbol_id != external.symbol_id) {
    return false;
  }

  const std::int64_t qty_diff =
      internal.qty_milli - external.qty_milli;
  if (qty_diff < 0) {
    if (-qty_diff > config_.qty_tolerance_milli) {
      return false;
    }
  } else if (qty_diff > config_.qty_tolerance_milli) {
    return false;
  }

  const std::int64_t amt_diff =
      internal.amount_cents - external.amount_cents;
  if (amt_diff < 0) {
    if (-amt_diff > config_.amount_tolerance_cents) {
      return false;
    }
  } else if (amt_diff > config_.amount_tolerance_cents) {
    return false;
  }

  return true;
}

ReconcileMatch ReconcileJournal::AttemptMatch(JournalLine* internal,
                                              JournalLine* external) {
  ReconcileMatch match{};
  if (internal == nullptr || external == nullptr) {
    return match;
  }

  match.internal_line_id = internal->line_id;
  match.external_line_id = external->line_id;
  match.qty_delta = internal->qty_milli - external->qty_milli;
  match.amount_delta = internal->amount_cents - external->amount_cents;

  if (match.qty_delta == 0 && match.amount_delta == 0) {
    match.resulting_state = ReconcileState::kMatched;
    internal->state = ReconcileState::kMatched;
    external->state = ReconcileState::kMatched;
  } else if (std::abs(match.qty_delta) <=
                 static_cast<std::int64_t>(config_.qty_tolerance_milli) &&
             std::abs(match.amount_delta) <= config_.amount_tolerance_cents) {
    match.resulting_state = ReconcileState::kAdjusted;
    internal->state = ReconcileState::kAdjusted;
    external->state = ReconcileState::kAdjusted;
  } else {
    match.resulting_state = ReconcileState::kUnmatched;
  }

  return match;
}

ReconcileJournalResult ReconcileJournal::Reconcile() {
  ReconcileJournalResult result{};
  result.status = Status::kOk;
  result.entries = lines_;

  std::int64_t net_qty = 0;
  std::int64_t net_amt = 0;

  for (JournalLine& line : lines_) {
    net_qty += line.qty_milli;
    net_amt += line.amount_cents;

    if (line.state == ReconcileState::kPending) {
      ++result.unmatched_count;
    } else if (line.state == ReconcileState::kMatched ||
               line.state == ReconcileState::kAdjusted) {
      ++result.matched_count;
    }
  }

  if (config_.auto_match_exact && net_qty == 0 && net_amt == 0) {
    for (JournalLine& line : lines_) {
      if (line.state == ReconcileState::kPending) {
        line.state = ReconcileState::kMatched;
        ++result.matched_count;
        --result.unmatched_count;
      }
    }
  }

  result.net_qty_milli = net_qty;
  result.net_amount_cents = net_amt;
  return result;
}

ReconcileJournalResult ReconcileJournal::MatchExternal(
    const std::vector<JournalLine>& external_lines) {
  ReconcileJournalResult result{};
  result.status = Status::kOk;

  std::vector<JournalLine> externals = external_lines;

  for (JournalLine& internal : lines_) {
    if (internal.state != ReconcileState::kPending) {
      continue;
    }

    for (JournalLine& external : externals) {
      if (external.state != ReconcileState::kPending) {
        continue;
      }
      if (!LinesMatch(internal, external)) {
        continue;
      }

      ReconcileMatch match = AttemptMatch(&internal, &external);
      result.matches.push_back(match);

      if (match.resulting_state == ReconcileState::kMatched ||
          match.resulting_state == ReconcileState::kAdjusted) {
        ++result.matched_count;
      } else {
        ++result.unmatched_count;
      }
      break;
    }
  }

  for (const JournalLine& line : lines_) {
    result.net_qty_milli += line.qty_milli;
    result.net_amount_cents += line.amount_cents;
  }

  result.entries = lines_;
  return result;
}

ReconcileJournalResult ReconcileJournal::SummarizeByAccount() {
  ReconcileJournalResult result{};
  result.status = Status::kOk;

  std::unordered_map<std::uint32_t, JournalLine> by_account;
  for (const JournalLine& line : lines_) {
    JournalLine& agg = by_account[line.account_id];
    agg.account_id = line.account_id;
    agg.qty_milli += line.qty_milli;
    agg.amount_cents += line.amount_cents;
  }

  for (const auto& pair : by_account) {
    result.entries.push_back(pair.second);
    result.net_qty_milli += pair.second.qty_milli;
    result.net_amount_cents += pair.second.amount_cents;
  }

  return result;
}

Status ReconcileJournal::WriteOffUnmatched() {
  for (JournalLine& line : lines_) {
    if (line.state == ReconcileState::kUnmatched ||
        line.state == ReconcileState::kPending) {
      line.state = ReconcileState::kWrittenOff;
      line.kind = JournalEntryKind::kReconcileAdjustment;
    }
  }
  return Status::kOk;
}

std::vector<JournalLine> ReconcileJournal::PendingLines() const {
  std::vector<JournalLine> pending;
  for (const JournalLine& line : lines_) {
    if (line.state == ReconcileState::kPending) {
      pending.push_back(line);
    }
  }
  return pending;
}

ReconcileJournalResult ReconcileJournal::VerifyBalanced() {
  ReconcileJournalResult result = Reconcile();
  if (result.net_qty_milli != 0 || result.net_amount_cents != 0) {
    result.status = Status::kBoundsError;
  }
  return result;
}

ReconcileJournalResult ReconcileJournal::FilterByBatch(
    std::uint32_t batch_id) const {
  ReconcileJournalResult result{};
  result.status = Status::kOk;
  for (const JournalLine& line : lines_) {
    if (line.batch_id == batch_id) {
      result.entries.push_back(line);
      result.net_qty_milli += line.qty_milli;
      result.net_amount_cents += line.amount_cents;
      if (line.state == ReconcileState::kMatched ||
          line.state == ReconcileState::kAdjusted) {
        ++result.matched_count;
      } else {
        ++result.unmatched_count;
      }
    }
  }
  return result;
}

Status ReconcileJournal::MarkMatched(std::uint32_t line_id) {
  for (JournalLine& line : lines_) {
    if (line.line_id == line_id) {
      line.state = ReconcileState::kMatched;
      return Status::kOk;
    }
  }
  return Status::kBoundsError;
}

void ReconcileJournal::Reset() {
  lines_.clear();
  next_line_id_ = 1;
}

}  // namespace ledger
}  // namespace tkr
