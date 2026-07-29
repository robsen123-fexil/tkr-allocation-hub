#include "tkr/desk/portfolio_snapshot.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace tkr {
namespace desk {

PortfolioSnapshotBuilder::PortfolioSnapshotBuilder(std::uint32_t desk_id,
                                                     std::int64_t desk_limit_cents)
    : desk_id_(desk_id),
      desk_limit_cents_(desk_limit_cents),
      nav_(25, 150),
      exposure_(desk_id, true) {}

PortfolioSnapshot PortfolioSnapshotBuilder::Build(const BatchWireFrame& frame) {
  PortfolioSnapshot snap{};
  snap.status = Status::kOk;

  const NavSnapshot nav = nav_.Compute(frame);
  if (nav.status != Status::kOk) {
    snap.status = nav.status;
    return snap;
  }

  const ExposureReport exposure = exposure_.Aggregate(frame);
  if (exposure.status != Status::kOk) {
    snap.status = exposure.status;
    return snap;
  }

  std::map<std::uint32_t, PortfolioSnapshotLine> merged;
  for (const NavLine& line : nav.lines) {
    PortfolioSnapshotLine row{};
    row.account_id = line.account_id;
    row.nav_cents = line.ending_nav_cents;
    row.fee_drag_cents = line.fee_accrual_cents;
    row.haircut_drag_cents = line.haircut_cents;
    merged[row.account_id] = row;
  }

  for (const ExposureSlice& slice : exposure.slices) {
    PortfolioSnapshotLine& row = merged[slice.key.account_id];
    if (row.account_id == 0) {
      row.account_id = slice.key.account_id;
    }
    row.gross_exposure_cents += slice.gross_notional_cents;
    row.net_qty_milli += slice.net_qty_milli;
  }

  snap.lines.reserve(merged.size());
  for (auto& entry : merged) {
    snap.fund_nav_cents += entry.second.nav_cents;
    snap.fund_exposure_cents += entry.second.gross_exposure_cents;
    snap.lines.push_back(entry.second);
  }

  for (PortfolioSnapshotLine& line : snap.lines) {
    line.concentration_bp =
        ComputeConcentrationBp(line.gross_exposure_cents, snap.fund_exposure_cents);
  }

  snap.utilization_bp =
      exposure_.ComputeDeskLimitUtilizationBp(exposure, desk_limit_cents_);

  std::ostringstream tag;
  tag << "desk=" << desk_id_ << " accts=" << snap.lines.size()
      << " util_bp=" << snap.utilization_bp;
  snap.summary_tag = tag.str();

  std::sort(snap.lines.begin(), snap.lines.end(),
            [](const PortfolioSnapshotLine& a, const PortfolioSnapshotLine& b) {
              return a.account_id < b.account_id;
            });
  return snap;
}

PortfolioSnapshot PortfolioSnapshotBuilder::BuildRolling(
    const BatchWireFrame& frame, const NavSnapshot& prior_nav) {
  nav_.RollForward(prior_nav, frame);
  return Build(frame);
}

bool PortfolioSnapshotBuilder::ExceedsRiskBudget(
    const PortfolioSnapshot& snap, std::int64_t max_utilization_bp) const {
  return snap.utilization_bp > max_utilization_bp;
}

std::vector<PortfolioSnapshotLine> PortfolioSnapshotBuilder::TopConcentrations(
    const PortfolioSnapshot& snap, std::size_t limit) const {
  std::vector<PortfolioSnapshotLine> ranked = snap.lines;
  std::sort(ranked.begin(), ranked.end(),
            [](const PortfolioSnapshotLine& a, const PortfolioSnapshotLine& b) {
              return a.concentration_bp > b.concentration_bp;
            });
  if (limit > 0 && ranked.size() > limit) {
    ranked.resize(limit);
  }
  return ranked;
}

std::int64_t PortfolioSnapshotBuilder::ComputeConcentrationBp(
    std::int64_t line_exposure, std::int64_t fund_exposure) {
  if (fund_exposure <= 0) {
    return 0;
  }
  return (line_exposure * 10000) / fund_exposure;
}

}  // namespace desk
}  // namespace tkr
