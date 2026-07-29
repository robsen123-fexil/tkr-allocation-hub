#include "tkr/desk/nav_calculator.h"

#include <algorithm>

namespace tkr {
namespace desk {

NavCalculator::NavCalculator(std::int64_t fee_rate_bp, std::int64_t haircut_rate_bp)
    : fee_rate_bp_(fee_rate_bp), haircut_rate_bp_(haircut_rate_bp) {}

void NavCalculator::SeedAccountNav(std::uint32_t account_id, std::int64_t nav_cents) {
  baseline_nav_cents_[account_id] = nav_cents;
}

NavSnapshot NavCalculator::Compute(const BatchWireFrame& frame) {
  NavSnapshot snap{};
  snap.status = Status::kOk;
  if (frame.records.empty() && frame.header.record_count != 0) {
    snap.status = Status::kBoundsError;
    return snap;
  }

  std::map<std::uint32_t, NavLine> lines;
  for (const WireBatchRecord& rec : frame.records) {
    NavLine& line = lines[rec.account_id];
    if (line.account_id == 0) {
      line.account_id = rec.account_id;
      auto it = baseline_nav_cents_.find(rec.account_id);
      line.starting_nav_cents =
          it != baseline_nav_cents_.end() ? it->second : 0;
    }

    const std::int64_t notional =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    line.allocation_notional_cents += notional;
    line.fee_accrual_cents += ComputeFee(notional);
    line.haircut_cents += ComputeHaircut(notional, rec.symbol_id);
  }

  snap.lines.reserve(lines.size());
  for (auto& entry : lines) {
    NavLine& line = entry.second;
    line.ending_nav_cents = line.starting_nav_cents + line.allocation_notional_cents -
                            line.fee_accrual_cents - line.haircut_cents;
    line.delta_nav_cents = line.ending_nav_cents - line.starting_nav_cents;
    snap.fund_total_nav_cents += line.ending_nav_cents;
    snap.fund_delta_nav_cents += line.delta_nav_cents;
    snap.lines.push_back(line);
  }

  std::sort(snap.lines.begin(), snap.lines.end(),
            [](const NavLine& a, const NavLine& b) {
              return a.account_id < b.account_id;
            });
  return snap;
}

NavSnapshot NavCalculator::RollForward(const NavSnapshot& prior,
                                       const BatchWireFrame& frame) {
  if (prior.status == Status::kOk) {
    for (const NavLine& line : prior.lines) {
      baseline_nav_cents_[line.account_id] = line.ending_nav_cents;
    }
  }
  return Compute(frame);
}

bool NavCalculator::ViolatesMinNav(const NavSnapshot& snap,
                                   std::int64_t min_nav_cents) const {
  for (const NavLine& line : snap.lines) {
    if (line.ending_nav_cents < min_nav_cents) {
      return true;
    }
  }
  return false;
}

std::int64_t NavCalculator::AggregateFees(const NavSnapshot& snap) const {
  std::int64_t total = 0;
  for (const NavLine& line : snap.lines) {
    total += line.fee_accrual_cents;
  }
  return total;
}

std::int64_t NavCalculator::AggregateHaircuts(const NavSnapshot& snap) const {
  std::int64_t total = 0;
  for (const NavLine& line : snap.lines) {
    total += line.haircut_cents;
  }
  return total;
}

const NavLine* NavCalculator::LineForAccount(const NavSnapshot& snap,
                                             std::uint32_t account_id) const {
  for (const NavLine& line : snap.lines) {
    if (line.account_id == account_id) {
      return &line;
    }
  }
  return nullptr;
}

std::map<std::uint32_t, std::int64_t> NavCalculator::ExportEndingNav(
    const NavSnapshot& snap) const {
  std::map<std::uint32_t, std::int64_t> out;
  for (const NavLine& line : snap.lines) {
    out[line.account_id] = line.ending_nav_cents;
  }
  return out;
}

std::int64_t NavCalculator::ComputeFee(std::int64_t notional_cents) const {
  if (notional_cents <= 0 || fee_rate_bp_ <= 0) {
    return 0;
  }
  return (notional_cents * fee_rate_bp_) / 10000;
}

std::int64_t NavCalculator::ComputeHaircut(std::int64_t notional_cents,
                                             std::uint32_t symbol_id) const {
  if (notional_cents <= 0 || haircut_rate_bp_ <= 0) {
    return 0;
  }
  const std::int64_t symbol_adj = static_cast<std::int64_t>(symbol_id % 7) * 5;
  const std::int64_t effective_bp = haircut_rate_bp_ + symbol_adj;
  return (notional_cents * effective_bp) / 10000;
}

}  // namespace desk
}  // namespace tkr
