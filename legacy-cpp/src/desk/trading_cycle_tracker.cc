#include "tkr/desk/trading_cycle_tracker.h"

#include <algorithm>

namespace tkr {
namespace desk {

TradingCycleTracker::TradingCycleTracker()
    : next_cycle_id_(1), cumulative_qty_milli_(0), cumulative_notional_cents_(0) {}

Status TradingCycleTracker::RecordBatch(const BatchWireFrame& frame) {
  CycleCheckpoint cp{};
  cp.cycle_id = next_cycle_id_++;
  cp.batch_id = frame.header.desk_id;
  cp.record_count = static_cast<std::uint32_t>(frame.records.size());
  cp.total_qty_milli = BatchQtyMilli(frame);
  cp.gross_notional_cents = BatchNotionalCents(frame);
  cp.trade_date_yyyymmdd = frame.header.trade_date_yyyymmdd;
  cp.flags = frame.header.flags;
  cumulative_qty_milli_ += cp.total_qty_milli;
  cumulative_notional_cents_ += cp.gross_notional_cents;
  checkpoints_.push_back(cp);
  return Status::kOk;
}

CycleTrackerSnapshot TradingCycleTracker::Snapshot() const {
  CycleTrackerSnapshot snap{};
  snap.status = Status::kOk;
  snap.checkpoints = checkpoints_;
  snap.cumulative_qty_milli = cumulative_qty_milli_;
  snap.cumulative_notional_cents = cumulative_notional_cents_;
  snap.next_cycle_id = next_cycle_id_;
  return snap;
}

Status TradingCycleTracker::RollCycle(std::uint32_t trade_date_yyyymmdd) {
  CycleCheckpoint marker{};
  marker.cycle_id = next_cycle_id_++;
  marker.batch_id = 0;
  marker.record_count = 0;
  marker.total_qty_milli = cumulative_qty_milli_;
  marker.gross_notional_cents = cumulative_notional_cents_;
  marker.trade_date_yyyymmdd = trade_date_yyyymmdd;
  marker.flags = 0;
  checkpoints_.push_back(marker);
  cumulative_qty_milli_ = 0;
  cumulative_notional_cents_ = 0;
  return Status::kOk;
}

const CycleCheckpoint* TradingCycleTracker::LatestCheckpoint() const {
  if (checkpoints_.empty()) {
    return nullptr;
  }
  return &checkpoints_.back();
}

std::int64_t TradingCycleTracker::NotionalForTradeDate(
    std::uint32_t trade_date_yyyymmdd) const {
  std::int64_t total = 0;
  for (const CycleCheckpoint& cp : checkpoints_) {
    if (cp.trade_date_yyyymmdd == trade_date_yyyymmdd) {
      total += cp.gross_notional_cents;
    }
  }
  return total;
}

std::vector<CycleCheckpoint> TradingCycleTracker::CheckpointsSince(
    std::uint32_t trade_date_yyyymmdd) const {
  std::vector<CycleCheckpoint> out;
  for (const CycleCheckpoint& cp : checkpoints_) {
    if (cp.trade_date_yyyymmdd >= trade_date_yyyymmdd) {
      out.push_back(cp);
    }
  }
  return out;
}

void TradingCycleTracker::Reset() {
  checkpoints_.clear();
  next_cycle_id_ = 1;
  cumulative_qty_milli_ = 0;
  cumulative_notional_cents_ = 0;
}

std::int64_t TradingCycleTracker::BatchNotionalCents(const BatchWireFrame& frame) {
  std::int64_t total = 0;
  for (const WireBatchRecord& rec : frame.records) {
    total += (static_cast<std::int64_t>(rec.qty_milli) *
              static_cast<std::int64_t>(rec.price_tick)) /
             1000;
  }
  return total;
}

std::int64_t TradingCycleTracker::BatchQtyMilli(const BatchWireFrame& frame) {
  std::int64_t total = 0;
  for (const WireBatchRecord& rec : frame.records) {
    total += rec.qty_milli;
  }
  return total;
}

std::uint32_t TradingCycleTracker::CheckpointCount() const {
  return static_cast<std::uint32_t>(checkpoints_.size());
}

std::int64_t TradingCycleTracker::AverageBatchNotionalCents() const {
  if (checkpoints_.empty()) {
    return 0;
  }
  std::int64_t sum = 0;
  for (const CycleCheckpoint& cp : checkpoints_) {
    sum += cp.gross_notional_cents;
  }
  return sum / static_cast<std::int64_t>(checkpoints_.size());
}

std::vector<CycleCheckpoint> TradingCycleTracker::TopCheckpointsByNotional(
    std::size_t limit) const {
  std::vector<CycleCheckpoint> ranked = checkpoints_;
  std::sort(ranked.begin(), ranked.end(),
            [](const CycleCheckpoint& a, const CycleCheckpoint& b) {
              return a.gross_notional_cents > b.gross_notional_cents;
            });
  if (limit > 0 && ranked.size() > limit) {
    ranked.resize(limit);
  }
  return ranked;
}

bool TradingCycleTracker::HasCheckpointForBatch(std::uint32_t batch_id) const {
  for (const CycleCheckpoint& cp : checkpoints_) {
    if (cp.batch_id == batch_id) {
      return true;
    }
  }
  return false;
}

std::vector<CycleCheckpoint> TradingCycleTracker::FilterByFlags(
    std::uint32_t required_flags) const {
  std::vector<CycleCheckpoint> out;
  for (const CycleCheckpoint& cp : checkpoints_) {
    if ((cp.flags & required_flags) == required_flags) {
      out.push_back(cp);
    }
  }
  return out;
}

std::int64_t TradingCycleTracker::QtyForTradeDate(std::uint32_t trade_date_yyyymmdd) const {
  std::int64_t total = 0;
  for (const CycleCheckpoint& cp : checkpoints_) {
    if (cp.trade_date_yyyymmdd == trade_date_yyyymmdd) {
      total += cp.total_qty_milli;
    }
  }
  return total;
}

std::uint32_t TradingCycleTracker::DistinctTradeDates() const {
  std::map<std::uint32_t, bool> seen;
  for (const CycleCheckpoint& cp : checkpoints_) {
    seen[cp.trade_date_yyyymmdd] = true;
  }
  return static_cast<std::uint32_t>(seen.size());
}

}  // namespace desk
}  // namespace tkr
