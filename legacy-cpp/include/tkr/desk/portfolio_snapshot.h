#pragma once

#include "tkr/desk/exposure_aggregator.h"
#include "tkr/desk/nav_calculator.h"
#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tkr {
namespace desk {

struct PortfolioSnapshotLine {
  std::uint32_t account_id;
  std::int64_t nav_cents;
  std::int64_t gross_exposure_cents;
  std::int64_t net_qty_milli;
  std::int64_t fee_drag_cents;
  std::int64_t haircut_drag_cents;
  std::int64_t concentration_bp;
};

struct PortfolioSnapshot {
  Status status;
  std::vector<PortfolioSnapshotLine> lines;
  std::int64_t fund_nav_cents;
  std::int64_t fund_exposure_cents;
  std::int64_t utilization_bp;
  std::string summary_tag;
};

class PortfolioSnapshotBuilder {
 public:
  PortfolioSnapshotBuilder(std::uint32_t desk_id, std::int64_t desk_limit_cents);

  PortfolioSnapshot Build(const BatchWireFrame& frame);
  PortfolioSnapshot BuildRolling(const BatchWireFrame& frame,
                                 const NavSnapshot& prior_nav);
  bool ExceedsRiskBudget(const PortfolioSnapshot& snap,
                         std::int64_t max_utilization_bp) const;
  std::vector<PortfolioSnapshotLine> TopConcentrations(
      const PortfolioSnapshot& snap, std::size_t limit) const;

 private:
  static std::int64_t ComputeConcentrationBp(std::int64_t line_exposure,
                                             std::int64_t fund_exposure);

  std::uint32_t desk_id_;
  std::int64_t desk_limit_cents_;
  NavCalculator nav_;
  ExposureAggregator exposure_;
};

}  // namespace desk
}  // namespace tkr
