#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

struct BenchmarkConstituent {
  std::uint32_t symbol_id;
  std::uint32_t weight_bp;
  std::int64_t benchmark_return_bp;
};

struct PortfolioHolding {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t qty_milli;
  std::int64_t market_value_cents;
};

struct TrackingErrorEntry {
  std::uint32_t symbol_id;
  std::int64_t portfolio_weight_bp;
  std::int64_t benchmark_weight_bp;
  std::int64_t active_weight_bp;
  std::int64_t contribution_bp;
};

struct BenchmarkTrackerConfig {
  std::uint32_t benchmark_id;
  std::uint32_t rebalance_threshold_bp;
};

struct BenchmarkTrackerResult {
  Status status;
  std::int64_t portfolio_return_bp;
  std::int64_t benchmark_return_bp;
  std::int64_t active_return_bp;
  std::int64_t tracking_error_bp;
  std::vector<TrackingErrorEntry> constituents;
  bool rebalance_needed;
};

class BenchmarkTracker {
 public:
  explicit BenchmarkTracker(BenchmarkTrackerConfig config);

  void SetBenchmark(const std::vector<BenchmarkConstituent>& constituents);
  void ClearBenchmark();

  BenchmarkTrackerResult Track(const std::vector<PortfolioHolding>& holdings);
  BenchmarkTrackerResult TrackBatch(const BatchWireFrame& frame);

  static std::int64_t ComputeWeightedReturn(
      const std::vector<PortfolioHolding>& holdings,
      const std::vector<BenchmarkConstituent>& benchmark);
  static std::int64_t ComputeTrackingError(
      const std::vector<TrackingErrorEntry>& entries);

 private:
  std::vector<TrackingErrorEntry> BuildActiveWeights(
      const std::vector<PortfolioHolding>& holdings) const;
  std::int64_t LookupBenchmarkWeight(std::uint32_t symbol_id) const;
  std::int64_t LookupBenchmarkReturn(std::uint32_t symbol_id) const;
  std::int64_t ComputePortfolioWeight(
      const PortfolioHolding& holding,
      std::int64_t total_value_cents) const;

  BenchmarkTrackerConfig config_;
  std::vector<BenchmarkConstituent> benchmark_;
};

}  // namespace desk
}  // namespace tkr
