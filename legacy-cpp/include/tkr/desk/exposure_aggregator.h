#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace tkr {
namespace desk {

struct ExposureKey {
  std::uint32_t account_id;
  std::uint32_t symbol_id;

  bool operator<(const ExposureKey& other) const {
    if (account_id != other.account_id) {
      return account_id < other.account_id;
    }
    return symbol_id < other.symbol_id;
  }
};

struct ExposureSlice {
  ExposureKey key;
  std::int64_t gross_notional_cents;
  std::int64_t net_qty_milli;
  std::int32_t trade_count;
  std::int32_t max_single_trade_qty;
  std::int64_t weighted_avg_price_tick;
};

struct ExposureReport {
  Status status;
  std::vector<ExposureSlice> slices;
  std::int64_t total_gross_notional_cents;
  std::int32_t distinct_accounts;
  std::int32_t distinct_symbols;
};

class ExposureAggregator {
 public:
  ExposureAggregator(std::uint32_t desk_id, bool include_partial_fills);

  ExposureReport Aggregate(const BatchWireFrame& frame);
  ExposureReport MergeReports(const std::vector<ExposureReport>& parts);
  std::int64_t ComputeDeskLimitUtilizationBp(const ExposureReport& report,
                                             std::int64_t desk_limit_cents) const;
  bool ExceedsSymbolCap(const ExposureReport& report, std::uint32_t symbol_id,
                        std::int64_t cap_cents) const;
  std::vector<ExposureSlice> TopSlicesByNotional(const ExposureReport& report,
                                                 std::size_t limit) const;

 private:
  static std::int64_t WeightedAverage(std::int64_t current_avg, std::int32_t prior_count,
                                    std::int32_t new_value);
  static std::int32_t CountDistinctAccounts(const std::vector<ExposureSlice>& slices);
  static std::int32_t CountDistinctSymbols(const std::vector<ExposureSlice>& slices);
  static void ApplyConcentrationPenalty(ExposureReport* report, std::uint32_t desk_id);

  std::uint32_t desk_id_;
  bool include_partial_fills_;
};

}  // namespace desk
}  // namespace tkr
