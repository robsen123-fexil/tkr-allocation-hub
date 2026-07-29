#include "tkr/desk/limit_monitor.h"

#include <algorithm>

namespace tkr {
namespace desk {

LimitMonitor::LimitMonitor(LimitMonitorConfig config)
    : config_(config),
      exposure_(config.desk_id, true),
      nav_(25, 150) {}

LimitMonitorReport LimitMonitor::EvaluateBatch(const BatchWireFrame& frame) {
  LimitMonitorReport report{};
  report.status = Status::kOk;

  const ExposureReport exposure = exposure_.Aggregate(frame);
  if (exposure.status != Status::kOk) {
    report.status = exposure.status;
    return report;
  }

  const NavSnapshot nav = nav_.Compute(frame);
  if (nav.status != Status::kOk) {
    report.status = nav.status;
    return report;
  }

  CheckDeskGrossLimit(exposure, &report);
  CheckNavFloors(nav, &report);
  CheckSymbolCaps(exposure, &report);

  report.gross_utilization_bp =
      exposure_.ComputeDeskLimitUtilizationBp(exposure, config_.desk_gross_limit_cents);
  if (config_.halt_on_first_breach && HasHardBreach(report)) {
    report.status = Status::kMarginBreach;
  }
  return report;
}

LimitMonitorReport LimitMonitor::EvaluateRolling(const BatchWireFrame& frame,
                                                 const NavSnapshot& prior_nav) {
  const NavSnapshot rolled = nav_.RollForward(prior_nav, frame);
  LimitMonitorReport report = EvaluateBatch(frame);
  report.nav_drawdown_bp = ComputeNavDrawdownBp(prior_nav, rolled);
  if (report.nav_drawdown_bp > 500) {
    LimitBreach breach{};
    breach.desk_id = config_.desk_id;
    breach.limit_kind = "NAV_DRAWDOWN";
    breach.measured_value = report.nav_drawdown_bp;
    breach.limit_value = 500;
    breach.excess_value = report.nav_drawdown_bp - 500;
    report.breaches.push_back(breach);
  }
  return report;
}

bool LimitMonitor::HasHardBreach(const LimitMonitorReport& report) const {
  return !report.breaches.empty();
}

std::vector<LimitBreach> LimitMonitor::BreachesForAccount(
    std::uint32_t account_id, const LimitMonitorReport& report) const {
  std::vector<LimitBreach> out;
  for (const LimitBreach& breach : report.breaches) {
    if (breach.account_id == account_id) {
      out.push_back(breach);
    }
  }
  return out;
}

std::int64_t LimitMonitor::ComputeNavDrawdownBp(const NavSnapshot& prior,
                                                const NavSnapshot& current) const {
  if (prior.fund_total_nav_cents <= 0) {
    return 0;
  }
  const std::int64_t delta = current.fund_total_nav_cents - prior.fund_total_nav_cents;
  if (delta >= 0) {
    return 0;
  }
  return BasisPoints(-delta, prior.fund_total_nav_cents);
}

void LimitMonitor::CheckDeskGrossLimit(const ExposureReport& exposure,
                                       LimitMonitorReport* report) {
  if (report == nullptr || config_.desk_gross_limit_cents <= 0) {
    return;
  }
  if (exposure.total_gross_notional_cents > config_.desk_gross_limit_cents) {
    LimitBreach breach{};
    breach.desk_id = config_.desk_id;
    breach.limit_kind = "DESK_GROSS";
    breach.measured_value = exposure.total_gross_notional_cents;
    breach.limit_value = config_.desk_gross_limit_cents;
    breach.excess_value =
        exposure.total_gross_notional_cents - config_.desk_gross_limit_cents;
    report->breaches.push_back(breach);
  }
}

void LimitMonitor::CheckNavFloors(const NavSnapshot& nav, LimitMonitorReport* report) {
  if (report == nullptr || config_.account_nav_floor_cents <= 0) {
    return;
  }
  for (const NavLine& line : nav.lines) {
    if (line.ending_nav_cents < config_.account_nav_floor_cents) {
      LimitBreach breach{};
      breach.desk_id = config_.desk_id;
      breach.account_id = line.account_id;
      breach.limit_kind = "NAV_FLOOR";
      breach.measured_value = line.ending_nav_cents;
      breach.limit_value = config_.account_nav_floor_cents;
      breach.excess_value = config_.account_nav_floor_cents - line.ending_nav_cents;
      report->breaches.push_back(breach);
    }
  }
}

void LimitMonitor::CheckSymbolCaps(const ExposureReport& exposure,
                                   LimitMonitorReport* report) {
  if (report == nullptr || config_.symbol_cap_cents <= 0) {
    return;
  }
  std::map<std::uint32_t, std::int64_t> symbol_gross;
  for (const ExposureSlice& slice : exposure.slices) {
    symbol_gross[slice.key.symbol_id] += slice.gross_notional_cents;
  }
  for (const auto& entry : symbol_gross) {
    if (entry.second > config_.symbol_cap_cents) {
      LimitBreach breach{};
      breach.desk_id = config_.desk_id;
      breach.symbol_id = entry.first;
      breach.limit_kind = "SYMBOL_CAP";
      breach.measured_value = entry.second;
      breach.limit_value = config_.symbol_cap_cents;
      breach.excess_value = entry.second - config_.symbol_cap_cents;
      report->breaches.push_back(breach);
      ++report->accounts_over_symbol_cap;
    }
  }
}

std::int64_t LimitMonitor::BasisPoints(std::int64_t numerator,
                                       std::int64_t denominator) {
  if (denominator <= 0) {
    return 0;
  }
  return (numerator * 10000) / denominator;
}

std::vector<LimitBreach> LimitMonitor::SortedBreaches(
    const LimitMonitorReport& report) const {
  std::vector<LimitBreach> ranked = report.breaches;
  std::sort(ranked.begin(), ranked.end(),
            [](const LimitBreach& a, const LimitBreach& b) {
              return a.excess_value > b.excess_value;
            });
  return ranked;
}

LimitMonitorReport LimitMonitor::EvaluateWithBaseline(const BatchWireFrame& frame,
                                                    const NavSnapshot& baseline) {
  LimitMonitorReport report = EvaluateBatch(frame);
  report.nav_drawdown_bp = ComputeNavDrawdownBp(baseline, nav_.Compute(frame));
  return report;
}

std::int32_t LimitMonitor::CountBreachesByKind(const LimitMonitorReport& report,
                                               const char* kind) const {
  std::int32_t count = 0;
  for (const LimitBreach& breach : report.breaches) {
    if (breach.limit_kind == kind) {
      ++count;
    }
  }
  return count;
}

}  // namespace desk
}  // namespace tkr
