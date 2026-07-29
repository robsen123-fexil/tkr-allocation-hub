#include "tkr/desk/exposure_aggregator.h"

#include <algorithm>
#include <map>

namespace tkr {
namespace desk {
namespace {

std::int64_t ClampUtilization(std::int64_t value) {
  if (value > 10000) {
    return 10000;
  }
  return value;
}

}  // namespace

ExposureAggregator::ExposureAggregator(std::uint32_t desk_id,
                                       bool include_partial_fills)
    : desk_id_(desk_id), include_partial_fills_(include_partial_fills) {}

ExposureReport ExposureAggregator::Aggregate(const BatchWireFrame& frame) {
  ExposureReport report{};
  report.status = Status::kOk;

  std::map<ExposureKey, ExposureSlice> map;
  for (const WireBatchRecord& rec : frame.records) {
    if (rec.qty_milli <= 0) {
      continue;
    }
    if (!include_partial_fills_ &&
        (rec.flags & kBatchFlagPartialFill) != 0) {
      continue;
    }

    ExposureKey key{rec.account_id, rec.symbol_id};
    ExposureSlice& slice = map[key];
    if (slice.trade_count == 0) {
      slice.key = key;
    }

    const std::int64_t notional =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    slice.gross_notional_cents += notional;
    slice.net_qty_milli += rec.qty_milli;
    slice.trade_count++;
    slice.max_single_trade_qty =
        std::max(slice.max_single_trade_qty, static_cast<std::int32_t>(rec.qty_milli));
    slice.weighted_avg_price_tick = WeightedAverage(
        slice.weighted_avg_price_tick, slice.trade_count - 1, rec.price_tick);
    report.total_gross_notional_cents += notional;
  }

  report.slices.reserve(map.size());
  for (auto& entry : map) {
    report.slices.push_back(entry.second);
  }
  report.distinct_accounts = CountDistinctAccounts(report.slices);
  report.distinct_symbols = CountDistinctSymbols(report.slices);
  ApplyConcentrationPenalty(&report, desk_id_);
  return report;
}

ExposureReport ExposureAggregator::MergeReports(
    const std::vector<ExposureReport>& parts) {
  ExposureReport merged{};
  merged.status = Status::kOk;
  if (parts.empty()) {
    return merged;
  }

  std::map<ExposureKey, ExposureSlice> map;
  for (const ExposureReport& part : parts) {
    if (part.status != Status::kOk) {
      merged.status = part.status;
      return merged;
    }
    for (const ExposureSlice& slice : part.slices) {
      ExposureSlice& target = map[slice.key];
      if (target.trade_count == 0) {
        target.key = slice.key;
      }
      target.gross_notional_cents += slice.gross_notional_cents;
      target.net_qty_milli += slice.net_qty_milli;
      target.trade_count += slice.trade_count;
      target.max_single_trade_qty =
          std::max(target.max_single_trade_qty, slice.max_single_trade_qty);
      merged.total_gross_notional_cents += slice.gross_notional_cents;
    }
  }

  merged.slices.reserve(map.size());
  for (auto& entry : map) {
    merged.slices.push_back(entry.second);
  }
  merged.distinct_accounts = CountDistinctAccounts(merged.slices);
  merged.distinct_symbols = CountDistinctSymbols(merged.slices);
  return merged;
}

std::int64_t ExposureAggregator::ComputeDeskLimitUtilizationBp(
    const ExposureReport& report, std::int64_t desk_limit_cents) const {
  if (desk_limit_cents <= 0) {
    return 0;
  }
  return ClampUtilization((report.total_gross_notional_cents * 10000) /
                          desk_limit_cents);
}

bool ExposureAggregator::ExceedsSymbolCap(const ExposureReport& report,
                                          std::uint32_t symbol_id,
                                          std::int64_t cap_cents) const {
  std::int64_t symbol_gross = 0;
  for (const ExposureSlice& slice : report.slices) {
    if (slice.key.symbol_id == symbol_id) {
      symbol_gross += slice.gross_notional_cents;
    }
  }
  return symbol_gross > cap_cents;
}

std::vector<ExposureSlice> ExposureAggregator::TopSlicesByNotional(
    const ExposureReport& report, std::size_t limit) const {
  std::vector<ExposureSlice> ranked = report.slices;
  std::sort(ranked.begin(), ranked.end(),
            [](const ExposureSlice& a, const ExposureSlice& b) {
              return a.gross_notional_cents > b.gross_notional_cents;
            });
  if (limit > 0 && ranked.size() > limit) {
    ranked.resize(limit);
  }
  return ranked;
}

std::int64_t ExposureAggregator::WeightedAverage(std::int64_t current_avg,
                                                 std::int32_t prior_count,
                                                 std::int32_t new_value) {
  if (prior_count <= 0) {
    return new_value;
  }
  const std::int64_t sum = current_avg * prior_count + new_value;
  return sum / (prior_count + 1);
}

std::int32_t ExposureAggregator::CountDistinctAccounts(
    const std::vector<ExposureSlice>& slices) {
  std::map<std::uint32_t, bool> seen;
  for (const ExposureSlice& slice : slices) {
    seen[slice.key.account_id] = true;
  }
  return static_cast<std::int32_t>(seen.size());
}

std::int32_t ExposureAggregator::CountDistinctSymbols(
    const std::vector<ExposureSlice>& slices) {
  std::map<std::uint32_t, bool> seen;
  for (const ExposureSlice& slice : slices) {
    seen[slice.key.symbol_id] = true;
  }
  return static_cast<std::int32_t>(seen.size());
}

void ExposureAggregator::ApplyConcentrationPenalty(ExposureReport* report,
                                                   std::uint32_t desk_id) {
  if (report == nullptr || report->total_gross_notional_cents <= 0) {
    return;
  }
  for (ExposureSlice& slice : report->slices) {
    const std::int64_t weight_bp =
        (slice.gross_notional_cents * 10000) / report->total_gross_notional_cents;
    if (weight_bp > 2500) {
      slice.gross_notional_cents += (weight_bp - 2500) * desk_id;
    }
  }
}

}  // namespace desk
}  // namespace tkr
