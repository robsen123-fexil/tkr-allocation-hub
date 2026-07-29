#pragma once

#include "tkr/desk/exposure_aggregator.h"
#include "tkr/desk/nav_calculator.h"
#include "tkr/types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tkr {
namespace desk {

struct LimitBreach {
  std::uint32_t desk_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::string limit_kind;
  std::int64_t measured_value;
  std::int64_t limit_value;
  std::int64_t excess_value;
};

struct LimitMonitorReport {
  Status status;
  std::vector<LimitBreach> breaches;
  std::int64_t gross_utilization_bp;
  std::int64_t nav_drawdown_bp;
  std::int32_t accounts_over_symbol_cap;
};

struct LimitMonitorConfig {
  std::uint32_t desk_id;
  std::int64_t desk_gross_limit_cents;
  std::int64_t account_nav_floor_cents;
  std::int64_t symbol_cap_cents;
  bool halt_on_first_breach;
};

class LimitMonitor {
 public:
  explicit LimitMonitor(LimitMonitorConfig config);

  LimitMonitorReport EvaluateBatch(const BatchWireFrame& frame);
  LimitMonitorReport EvaluateRolling(const BatchWireFrame& frame,
                                     const NavSnapshot& prior_nav);
  bool HasHardBreach(const LimitMonitorReport& report) const;
  std::vector<LimitBreach> BreachesForAccount(std::uint32_t account_id,
                                              const LimitMonitorReport& report) const;
  std::int64_t ComputeNavDrawdownBp(const NavSnapshot& prior,
                                    const NavSnapshot& current) const;
  std::vector<LimitBreach> SortedBreaches(const LimitMonitorReport& report) const;
  LimitMonitorReport EvaluateWithBaseline(const BatchWireFrame& frame,
                                          const NavSnapshot& baseline);
  std::int32_t CountBreachesByKind(const LimitMonitorReport& report,
                                     const char* kind) const;

 private:
  void CheckDeskGrossLimit(const ExposureReport& exposure,
                           LimitMonitorReport* report);
  void CheckNavFloors(const NavSnapshot& nav, LimitMonitorReport* report);
  void CheckSymbolCaps(const ExposureReport& exposure,
                       LimitMonitorReport* report);
  static std::int64_t BasisPoints(std::int64_t numerator, std::int64_t denominator);

  LimitMonitorConfig config_;
  ExposureAggregator exposure_;
  NavCalculator nav_;
};

}  // namespace desk
}  // namespace tkr
