#include "tkr/desk/allocation_reconciler.h"

#include <algorithm>

namespace tkr {
namespace desk {

ReconcileReport AllocationReconciler::ReconcileBatch(
    const BatchWireFrame& frame, const std::vector<ProRataSlice>& slices) {
  ReconcileReport report{};
  report.status = Status::kOk;

  std::map<std::uint32_t, std::int64_t> slice_qty;
  std::map<std::uint32_t, std::uint32_t> slice_price;
  for (const ProRataSlice& slice : slices) {
    slice_qty[slice.account_id] += slice.qty_milli;
    slice_price[slice.account_id] = 100;
  }

  std::map<std::uint32_t, std::int64_t> batch_qty;
  std::map<std::uint32_t, std::int64_t> batch_notional;
  for (const WireBatchRecord& rec : frame.records) {
    batch_qty[rec.account_id] += rec.qty_milli;
    batch_notional[rec.account_id] += RecordNotionalCents(rec);
  }

  for (const auto& entry : batch_qty) {
    const std::uint32_t account_id = entry.first;
    const std::int64_t expected = entry.second;
    const std::int64_t actual = slice_qty[account_id];
    if (expected == actual) {
      continue;
    }
    ReconcileDelta delta{};
    delta.account_id = account_id;
    delta.expected_qty_milli = expected;
    delta.actual_qty_milli = actual;
    delta.notional_delta_cents =
        batch_notional[account_id] -
        SliceNotionalCents(ProRataSlice{account_id, 0,
                                        static_cast<std::uint32_t>(actual),
                                        0},
                           slice_price[account_id]);
    report.deltas.push_back(delta);
    report.total_notional_delta_cents += delta.notional_delta_cents;
    ++report.mismatch_count;
  }

  for (const WireBatchRecord& rec : frame.records) {
    const std::int64_t slice_qty_for_account = slice_qty[rec.account_id];
    if (slice_qty_for_account <= 0) {
      ReconcileDelta delta{};
      delta.record_id = rec.record_id;
      delta.account_id = rec.account_id;
      delta.expected_qty_milli = rec.qty_milli;
      delta.actual_qty_milli = 0;
      delta.notional_delta_cents = RecordNotionalCents(rec);
      report.deltas.push_back(delta);
      report.total_notional_delta_cents += delta.notional_delta_cents;
      ++report.mismatch_count;
    }
  }

  std::sort(report.deltas.begin(), report.deltas.end(),
            [](const ReconcileDelta& a, const ReconcileDelta& b) {
              if (a.account_id != b.account_id) {
                return a.account_id < b.account_id;
              }
              return a.record_id < b.record_id;
            });
  return report;
}

ReconcileReport AllocationReconciler::ReconcileAgainstLedger(
    const BatchWireFrame& frame,
    const std::map<std::uint32_t, std::int64_t>& ledger_qty) {
  ReconcileReport report{};
  report.status = Status::kOk;

  std::map<std::uint32_t, std::int64_t> batch_qty;
  for (const WireBatchRecord& rec : frame.records) {
    batch_qty[rec.account_id] += rec.qty_milli;
  }

  for (const auto& entry : batch_qty) {
    const std::uint32_t account_id = entry.first;
    const std::int64_t expected = entry.second;
    auto it = ledger_qty.find(account_id);
    const std::int64_t actual = it != ledger_qty.end() ? it->second : 0;
    if (expected == actual) {
      continue;
    }
    ReconcileDelta delta{};
    delta.account_id = account_id;
    delta.expected_qty_milli = expected;
    delta.actual_qty_milli = actual;
    report.deltas.push_back(delta);
    ++report.mismatch_count;
  }
  return report;
}

bool AllocationReconciler::WithinTolerance(const ReconcileReport& report,
                                           std::int64_t tolerance_cents) const {
  if (tolerance_cents < 0) {
    return false;
  }
  const std::int64_t abs_delta = report.total_notional_delta_cents >= 0
                                     ? report.total_notional_delta_cents
                                     : -report.total_notional_delta_cents;
  return abs_delta <= tolerance_cents;
}

std::vector<ReconcileDelta> AllocationReconciler::TopMismatches(
    const ReconcileReport& report, std::size_t limit) const {
  std::vector<ReconcileDelta> ranked = report.deltas;
  std::sort(ranked.begin(), ranked.end(),
            [](const ReconcileDelta& a, const ReconcileDelta& b) {
              const std::int64_t aa = a.notional_delta_cents >= 0
                                          ? a.notional_delta_cents
                                          : -a.notional_delta_cents;
              const std::int64_t bb = b.notional_delta_cents >= 0
                                          ? b.notional_delta_cents
                                          : -b.notional_delta_cents;
              return aa > bb;
            });
  if (limit > 0 && ranked.size() > limit) {
    ranked.resize(limit);
  }
  return ranked;
}

std::int64_t AllocationReconciler::RecordNotionalCents(const WireBatchRecord& rec) {
  return (static_cast<std::int64_t>(rec.qty_milli) *
          static_cast<std::int64_t>(rec.price_tick)) /
         1000;
}

std::int64_t AllocationReconciler::SliceNotionalCents(const ProRataSlice& slice,
                                                      std::uint32_t price_tick) {
  return (static_cast<std::int64_t>(slice.qty_milli) *
          static_cast<std::int64_t>(price_tick)) /
         1000;
}

}  // namespace desk
}  // namespace tkr
