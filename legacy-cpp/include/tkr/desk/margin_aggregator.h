#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {

struct MarginLine {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t notional_cents;
  std::int64_t initial_margin_cents;
  std::int64_t maintenance_margin_cents;
  std::uint32_t qty_milli;
};

struct MarginAggregatorConfig {
  std::uint32_t desk_id;
  std::uint32_t margin_rate_bp;  // basis points of notional
};

struct MarginAggregatorResult {
  Status status;
  std::vector<MarginLine> lines;
  std::vector<MarginCheckResult> account_checks;
  std::uint32_t breach_count;
  std::int64_t total_initial_margin_cents;
};

class MarginAggregator {
 public:
  explicit MarginAggregator(MarginAggregatorConfig config);

  MarginAggregatorResult Aggregate(const BatchWireFrame& frame);

  void SetAccountAvailableMargin(std::uint32_t account_id,
                                 std::int64_t available_cents);
  void ClearAccountMargins();

  static std::int64_t ComputeNotionalCents(std::uint32_t qty_milli,
                                           std::uint32_t price_tick);
  static std::int64_t ComputeInitialMargin(std::int64_t notional_cents,
                                           std::uint32_t rate_bp);
  static std::int64_t ComputeMaintenanceMargin(std::int64_t initial_margin,
                                               std::uint32_t maint_ratio_bp);

 private:
  Status BuildMarginLines(const BatchWireFrame& frame,
                          std::vector<MarginLine>* lines) const;
  Status RollUpByAccount(const std::vector<MarginLine>& lines,
                         std::vector<MarginCheckResult>* checks) const;
  std::int64_t LookupAvailableMargin(std::uint32_t account_id) const;

  MarginAggregatorConfig config_;
  std::unordered_map<std::uint32_t, std::int64_t> account_available_;
};

}  // namespace desk
}  // namespace tkr
