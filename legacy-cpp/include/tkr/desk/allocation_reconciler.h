#pragma once

#include "tkr/desk/pro_rata_allocator.h"
#include "tkr/types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace tkr {
namespace desk {

struct ReconcileDelta {
  std::uint32_t record_id;
  std::uint32_t account_id;
  std::int64_t expected_qty_milli;
  std::int64_t actual_qty_milli;
  std::int64_t notional_delta_cents;
};

struct ReconcileReport {
  Status status;
  std::vector<ReconcileDelta> deltas;
  std::int64_t total_notional_delta_cents;
  std::int32_t mismatch_count;
};

class AllocationReconciler {
 public:
  ReconcileReport ReconcileBatch(const BatchWireFrame& frame,
                                 const std::vector<ProRataSlice>& slices);
  ReconcileReport ReconcileAgainstLedger(const BatchWireFrame& frame,
                                         const std::map<std::uint32_t, std::int64_t>& ledger_qty);
  bool WithinTolerance(const ReconcileReport& report, std::int64_t tolerance_cents) const;
  std::vector<ReconcileDelta> TopMismatches(const ReconcileReport& report,
                                            std::size_t limit) const;

 private:
  static std::int64_t RecordNotionalCents(const WireBatchRecord& rec);
  static std::int64_t SliceNotionalCents(const ProRataSlice& slice,
                                         std::uint32_t price_tick);
};

}  // namespace desk
}  // namespace tkr
