#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace tkr {
namespace desk {

struct NavLine {
  std::uint32_t account_id;
  std::int64_t starting_nav_cents;
  std::int64_t allocation_notional_cents;
  std::int64_t fee_accrual_cents;
  std::int64_t haircut_cents;
  std::int64_t ending_nav_cents;
  std::int64_t delta_nav_cents;
};

struct NavSnapshot {
  Status status;
  std::vector<NavLine> lines;
  std::int64_t fund_total_nav_cents;
  std::int64_t fund_delta_nav_cents;
};

class NavCalculator {
 public:
  NavCalculator(std::int64_t fee_rate_bp, std::int64_t haircut_rate_bp);

  void SeedAccountNav(std::uint32_t account_id, std::int64_t nav_cents);
  NavSnapshot Compute(const BatchWireFrame& frame);
  NavSnapshot RollForward(const NavSnapshot& prior, const BatchWireFrame& frame);
  bool ViolatesMinNav(const NavSnapshot& snap, std::int64_t min_nav_cents) const;
  std::int64_t AggregateFees(const NavSnapshot& snap) const;
  std::int64_t AggregateHaircuts(const NavSnapshot& snap) const;
  const NavLine* LineForAccount(const NavSnapshot& snap,
                                std::uint32_t account_id) const;
  std::map<std::uint32_t, std::int64_t> ExportEndingNav(
      const NavSnapshot& snap) const;

 private:
  std::int64_t ComputeFee(std::int64_t notional_cents) const;
  std::int64_t ComputeHaircut(std::int64_t notional_cents,
                              std::uint32_t symbol_id) const;

  std::map<std::uint32_t, std::int64_t> baseline_nav_cents_;
  std::int64_t fee_rate_bp_;
  std::int64_t haircut_rate_bp_;
};

}  // namespace desk
}  // namespace tkr
