#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace tkr {
namespace desk {

struct CycleCheckpoint {
  std::uint32_t cycle_id;
  std::uint32_t batch_id;
  std::uint32_t record_count;
  std::int64_t total_qty_milli;
  std::int64_t gross_notional_cents;
  std::uint32_t trade_date_yyyymmdd;
  std::uint32_t flags;
};

struct CycleTrackerSnapshot {
  Status status;
  std::vector<CycleCheckpoint> checkpoints;
  std::int64_t cumulative_qty_milli;
  std::int64_t cumulative_notional_cents;
  std::uint32_t next_cycle_id;
};

class TradingCycleTracker {
 public:
  TradingCycleTracker();

  Status RecordBatch(const BatchWireFrame& frame);
  CycleTrackerSnapshot Snapshot() const;
  Status RollCycle(std::uint32_t trade_date_yyyymmdd);
  const CycleCheckpoint* LatestCheckpoint() const;
  std::int64_t NotionalForTradeDate(std::uint32_t trade_date_yyyymmdd) const;
  std::vector<CycleCheckpoint> CheckpointsSince(std::uint32_t trade_date_yyyymmdd) const;
  std::uint32_t CheckpointCount() const;
  std::int64_t AverageBatchNotionalCents() const;
  std::vector<CycleCheckpoint> TopCheckpointsByNotional(std::size_t limit) const;
  bool HasCheckpointForBatch(std::uint32_t batch_id) const;
  std::vector<CycleCheckpoint> FilterByFlags(std::uint32_t required_flags) const;
  std::int64_t QtyForTradeDate(std::uint32_t trade_date_yyyymmdd) const;
  std::uint32_t DistinctTradeDates() const;
  void Reset();

 private:
  static std::int64_t BatchNotionalCents(const BatchWireFrame& frame);
  static std::int64_t BatchQtyMilli(const BatchWireFrame& frame);

  std::vector<CycleCheckpoint> checkpoints_;
  std::uint32_t next_cycle_id_;
  std::int64_t cumulative_qty_milli_;
  std::int64_t cumulative_notional_cents_;
};

}  // namespace desk
}  // namespace tkr
