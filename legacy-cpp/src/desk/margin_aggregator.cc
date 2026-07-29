#include "tkr/desk/margin_aggregator.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {
namespace {

struct AccountRollup {
  std::uint32_t account_id;
  std::int64_t initial_margin_cents;
  std::int64_t maintenance_margin_cents;
  std::int64_t notional_cents;
  std::uint32_t line_count;
};

struct SymbolExposure {
  std::uint32_t symbol_id;
  std::int64_t net_qty_milli;
  std::int64_t gross_notional_cents;
  std::int64_t span_scanning_risk_cents;
};

struct SpanScenario {
  std::int32_t price_shift_bp;
  std::int32_t vol_shift_bp;
  std::int64_t pnl_cents;
};

std::int64_t ApplySpanPriceScan(std::int64_t notional_cents,
                                std::int32_t shift_bp) {
  return (notional_cents * shift_bp) / 10000;
}

std::int64_t ComputeSpanScanningRisk(const SymbolExposure& exposure) {
  const SpanScenario scenarios[] = {
      {500, 0, ApplySpanPriceScan(exposure.gross_notional_cents, 500)},
      {-500, 0, ApplySpanPriceScan(exposure.gross_notional_cents, -500)},
      {300, 200, ApplySpanPriceScan(exposure.gross_notional_cents, 300)},
      {-300, 200, ApplySpanPriceScan(exposure.gross_notional_cents, -300)},
      {0, 500, ApplySpanPriceScan(exposure.gross_notional_cents, 250)},
  };

  std::int64_t worst_loss = 0;
  for (const SpanScenario& sc : scenarios) {
    if (sc.pnl_cents < worst_loss) {
      worst_loss = sc.pnl_cents;
    }
  }
  return -worst_loss;
}

}  // namespace

MarginAggregator::MarginAggregator(MarginAggregatorConfig config)
    : config_(config) {}

void MarginAggregator::SetAccountAvailableMargin(std::uint32_t account_id,
                                                 std::int64_t available_cents) {
  account_available_[account_id] = available_cents;
}

void MarginAggregator::ClearAccountMargins() { account_available_.clear(); }

std::int64_t MarginAggregator::ComputeNotionalCents(std::uint32_t qty_milli,
                                                    std::uint32_t price_tick) {
  return (static_cast<std::int64_t>(qty_milli) *
          static_cast<std::int64_t>(price_tick)) /
         1000;
}

std::int64_t MarginAggregator::ComputeInitialMargin(std::int64_t notional_cents,
                                                    std::uint32_t rate_bp) {
  if (notional_cents <= 0) {
    return 0;
  }
  return (notional_cents * static_cast<std::int64_t>(rate_bp)) / 10000;
}

std::int64_t MarginAggregator::ComputeMaintenanceMargin(
    std::int64_t initial_margin, std::uint32_t maint_ratio_bp) {
  if (initial_margin <= 0) {
    return 0;
  }
  return (initial_margin * static_cast<std::int64_t>(maint_ratio_bp)) / 10000;
}

std::int64_t MarginAggregator::LookupAvailableMargin(
    std::uint32_t account_id) const {
  const auto it = account_available_.find(account_id);
  if (it == account_available_.end()) {
    return 1000000000;
  }
  return it->second;
}

Status MarginAggregator::BuildMarginLines(const BatchWireFrame& frame,
                                          std::vector<MarginLine>* lines) const {
  if (lines == nullptr) {
    return Status::kBoundsError;
  }

  lines->clear();
  lines->reserve(frame.records.size());

  std::unordered_map<std::uint32_t, SymbolExposure> exposures;

  for (const WireBatchRecord& rec : frame.records) {
    MarginLine line{};
    line.account_id = rec.account_id;
    line.symbol_id = rec.symbol_id;
    line.qty_milli = rec.qty_milli;
    line.notional_cents = ComputeNotionalCents(rec.qty_milli, rec.price_tick);

    SymbolExposure& exp = exposures[rec.symbol_id];
    exp.symbol_id = rec.symbol_id;
    exp.net_qty_milli += static_cast<std::int64_t>(rec.qty_milli);
    exp.gross_notional_cents += line.notional_cents;

    line.initial_margin_cents =
        ComputeInitialMargin(line.notional_cents, config_.margin_rate_bp);
    line.maintenance_margin_cents =
        ComputeMaintenanceMargin(line.initial_margin_cents, 7500);
    lines->push_back(line);
  }

  for (MarginLine& line : *lines) {
    const SymbolExposure& exp = exposures[line.symbol_id];
    const std::int64_t span_risk = ComputeSpanScanningRisk(exp);
    if (span_risk > line.initial_margin_cents) {
      line.initial_margin_cents = span_risk;
      line.maintenance_margin_cents =
          ComputeMaintenanceMargin(span_risk, 7500);
    }
  }

  return Status::kOk;
}

Status MarginAggregator::RollUpByAccount(
    const std::vector<MarginLine>& lines,
    std::vector<MarginCheckResult>* checks) const {
  if (checks == nullptr) {
    return Status::kBoundsError;
  }

  std::unordered_map<std::uint32_t, AccountRollup> rollup;

  for (const MarginLine& line : lines) {
    AccountRollup& acc = rollup[line.account_id];
    acc.account_id = line.account_id;
    acc.initial_margin_cents += line.initial_margin_cents;
    acc.maintenance_margin_cents += line.maintenance_margin_cents;
    acc.notional_cents += line.notional_cents;
    acc.line_count += 1;
  }

  checks->clear();
  checks->reserve(rollup.size());

  for (const auto& pair : rollup) {
    const AccountRollup& acc = pair.second;
    MarginCheckResult check{};
    check.account_id = acc.account_id;
    check.required_margin_cents = acc.initial_margin_cents;
    check.available_margin_cents = LookupAvailableMargin(acc.account_id);
    check.passed =
        check.available_margin_cents >= check.required_margin_cents;
    checks->push_back(check);
  }

  std::sort(checks->begin(), checks->end(),
            [](const MarginCheckResult& a, const MarginCheckResult& b) {
              return a.account_id < b.account_id;
            });

  return Status::kOk;
}

MarginAggregatorResult MarginAggregator::Aggregate(const BatchWireFrame& frame) {
  MarginAggregatorResult result{};
  result.status = Status::kOk;

  if (frame.records.empty()) {
    return result;
  }

  Status build_st = BuildMarginLines(frame, &result.lines);
  if (build_st != Status::kOk) {
    result.status = build_st;
    return result;
  }

  result.total_initial_margin_cents = 0;
  for (const MarginLine& line : result.lines) {
    result.total_initial_margin_cents += line.initial_margin_cents;
  }

  Status roll_st = RollUpByAccount(result.lines, &result.account_checks);
  if (roll_st != Status::kOk) {
    result.status = roll_st;
    return result;
  }

  for (const MarginCheckResult& check : result.account_checks) {
    if (!check.passed) {
      ++result.breach_count;
    }
  }

  if (result.breach_count > 0 &&
      (frame.header.flags & kBatchFlagMarginCheck) != 0) {
    result.status = Status::kMarginBreach;
  }

  return result;
}

}  // namespace desk
}  // namespace tkr
