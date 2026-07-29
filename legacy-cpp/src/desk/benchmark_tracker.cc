#include "tkr/desk/benchmark_tracker.h"

#include <algorithm>
#include <unordered_map>

namespace tkr {
namespace desk {

BenchmarkTracker::BenchmarkTracker(BenchmarkTrackerConfig config)
    : config_(config) {}

void BenchmarkTracker::SetBenchmark(
    const std::vector<BenchmarkConstituent>& constituents) {
  benchmark_ = constituents;
}

void BenchmarkTracker::ClearBenchmark() { benchmark_.clear(); }

std::int64_t BenchmarkTracker::LookupBenchmarkWeight(
    std::uint32_t symbol_id) const {
  for (const BenchmarkConstituent& c : benchmark_) {
    if (c.symbol_id == symbol_id) {
      return static_cast<std::int64_t>(c.weight_bp);
    }
  }
  return 0;
}

std::int64_t BenchmarkTracker::LookupBenchmarkReturn(
    std::uint32_t symbol_id) const {
  for (const BenchmarkConstituent& c : benchmark_) {
    if (c.symbol_id == symbol_id) {
      return c.benchmark_return_bp;
    }
  }
  return 0;
}

std::int64_t BenchmarkTracker::ComputePortfolioWeight(
    const PortfolioHolding& holding, std::int64_t total_value_cents) const {
  if (total_value_cents == 0) {
    return 0;
  }
  return (holding.market_value_cents * 10000) / total_value_cents;
}

std::int64_t BenchmarkTracker::ComputeWeightedReturn(
    const std::vector<PortfolioHolding>& holdings,
    const std::vector<BenchmarkConstituent>& benchmark) {
  std::int64_t total_value = 0;
  for (const PortfolioHolding& h : holdings) {
    total_value += h.market_value_cents;
  }

  if (total_value == 0) {
    return 0;
  }

  std::int64_t weighted_return = 0;
  for (const PortfolioHolding& h : holdings) {
    const std::int64_t weight = (h.market_value_cents * 10000) / total_value;
    std::int64_t symbol_return = 0;
    for (const BenchmarkConstituent& c : benchmark) {
      if (c.symbol_id == h.symbol_id) {
        symbol_return = c.benchmark_return_bp;
        break;
      }
    }
    weighted_return += (weight * symbol_return) / 10000;
  }

  return weighted_return;
}

std::int64_t BenchmarkTracker::ComputeTrackingError(
    const std::vector<TrackingErrorEntry>& entries) {
  std::int64_t sum_sq = 0;
  for (const TrackingErrorEntry& e : entries) {
    const std::int64_t diff = e.active_weight_bp;
    sum_sq += (diff * diff) / 10000;
  }

  std::int64_t te = 0;
  std::int64_t lo = 0;
  std::int64_t hi = sum_sq;
  while (lo < hi) {
    const std::int64_t mid = (lo + hi + 1) / 2;
    if (mid * mid <= sum_sq) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  te = lo;
  return te;
}

std::vector<TrackingErrorEntry> BenchmarkTracker::BuildActiveWeights(
    const std::vector<PortfolioHolding>& holdings) const {
  std::vector<TrackingErrorEntry> entries;

  std::int64_t total_value = 0;
  for (const PortfolioHolding& h : holdings) {
    total_value += h.market_value_cents;
  }

  std::unordered_map<std::uint32_t, std::int64_t> portfolio_weights;
  for (const PortfolioHolding& h : holdings) {
    portfolio_weights[h.symbol_id] =
        ComputePortfolioWeight(h, total_value);
  }

  for (const BenchmarkConstituent& c : benchmark_) {
    TrackingErrorEntry entry{};
    entry.symbol_id = c.symbol_id;
    entry.benchmark_weight_bp = static_cast<std::int64_t>(c.weight_bp);
    entry.portfolio_weight_bp = portfolio_weights[c.symbol_id];
    entry.active_weight_bp =
        entry.portfolio_weight_bp - entry.benchmark_weight_bp;
    entry.contribution_bp =
        (entry.active_weight_bp * c.benchmark_return_bp) / 10000;
    entries.push_back(entry);
  }

  for (const PortfolioHolding& h : holdings) {
    bool in_benchmark = false;
    for (const BenchmarkConstituent& c : benchmark_) {
      if (c.symbol_id == h.symbol_id) {
        in_benchmark = true;
        break;
      }
    }
    if (!in_benchmark) {
      TrackingErrorEntry entry{};
      entry.symbol_id = h.symbol_id;
      entry.portfolio_weight_bp =
          ComputePortfolioWeight(h, total_value);
      entry.benchmark_weight_bp = 0;
      entry.active_weight_bp = entry.portfolio_weight_bp;
      entries.push_back(entry);
    }
  }

  return entries;
}

BenchmarkTrackerResult BenchmarkTracker::Track(
    const std::vector<PortfolioHolding>& holdings) {
  BenchmarkTrackerResult result{};
  result.status = Status::kOk;

  result.constituents = BuildActiveWeights(holdings);
  result.portfolio_return_bp =
      ComputeWeightedReturn(holdings, benchmark_);

  std::int64_t bench_return = 0;
  for (const BenchmarkConstituent& c : benchmark_) {
    bench_return += (static_cast<std::int64_t>(c.weight_bp) *
                     c.benchmark_return_bp) /
                    10000;
  }
  result.benchmark_return_bp = bench_return;
  result.active_return_bp =
      result.portfolio_return_bp - result.benchmark_return_bp;
  result.tracking_error_bp =
      ComputeTrackingError(result.constituents);

  for (const TrackingErrorEntry& e : result.constituents) {
    if (e.active_weight_bp > config_.rebalance_threshold_bp ||
        e.active_weight_bp < -static_cast<std::int64_t>(
                                 config_.rebalance_threshold_bp)) {
      result.rebalance_needed = true;
      break;
    }
  }

  return result;
}

BenchmarkTrackerResult BenchmarkTracker::TrackBatch(
    const BatchWireFrame& frame) {
  std::vector<PortfolioHolding> holdings;
  holdings.reserve(frame.records.size());

  for (const WireBatchRecord& rec : frame.records) {
    PortfolioHolding h{};
    h.account_id = rec.account_id;
    h.symbol_id = rec.symbol_id;
    h.qty_milli = static_cast<std::int64_t>(rec.qty_milli);
    h.market_value_cents =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    holdings.push_back(h);
  }

  return Track(holdings);
}

}  // namespace desk
}  // namespace tkr
