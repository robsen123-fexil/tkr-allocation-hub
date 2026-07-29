#include "tkr/desk/allocation_solver.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {
namespace {

std::uint32_t SumSliceQty(const std::vector<ProRataSlice>& slices) {
  std::uint32_t total = 0;
  for (const ProRataSlice& s : slices) {
    total += s.qty_milli;
  }
  return total;
}

const AllocationBound* FindBound(const std::vector<AllocationBound>& bounds,
                                 std::uint32_t account_id) {
  for (const AllocationBound& b : bounds) {
    if (b.account_id == account_id) {
      return &b;
    }
  }
  return nullptr;
}

struct BoundViolation {
  std::uint32_t account_id;
  std::uint32_t current_qty;
  std::uint32_t min_qty;
  std::uint32_t max_qty;
  std::int32_t deficit;
  std::int32_t surplus;
};

std::vector<BoundViolation> DetectViolations(
    const std::vector<ProRataSlice>& slices,
    const std::vector<AllocationBound>& bounds) {
  std::vector<BoundViolation> violations;
  for (const ProRataSlice& slice : slices) {
    const AllocationBound* bound = FindBound(bounds, slice.account_id);
    if (bound == nullptr) {
      continue;
    }
    BoundViolation v{};
    v.account_id = slice.account_id;
    v.current_qty = slice.qty_milli;
    v.min_qty = bound->min_qty_milli;
    v.max_qty = bound->max_qty_milli;
    if (slice.qty_milli < bound->min_qty_milli) {
      v.deficit = static_cast<std::int32_t>(bound->min_qty_milli - slice.qty_milli);
    }
    if (slice.qty_milli > bound->max_qty_milli) {
      v.surplus = static_cast<std::int32_t>(slice.qty_milli - bound->max_qty_milli);
    }
    if (v.deficit != 0 || v.surplus != 0) {
      violations.push_back(v);
    }
  }
  return violations;
}

}  // namespace

AllocationSolver::AllocationSolver(AllocationSolverConfig config)
    : config_(config) {}

void AllocationSolver::SetBound(std::uint32_t account_id, std::uint32_t min_qty,
                                std::uint32_t max_qty) {
  for (AllocationBound& b : custom_bounds_) {
    if (b.account_id == account_id) {
      b.min_qty_milli = min_qty;
      b.max_qty_milli = max_qty;
      return;
    }
  }
  AllocationBound bound{};
  bound.account_id = account_id;
  bound.min_qty_milli = min_qty;
  bound.max_qty_milli = max_qty;
  custom_bounds_.push_back(bound);
}

void AllocationSolver::ClearBounds() { custom_bounds_.clear(); }

Status AllocationSolver::ClampSlice(ProRataSlice* slice,
                                    const AllocationBound& bound) const {
  if (slice == nullptr) {
    return Status::kBoundsError;
  }
  if (slice->qty_milli < bound.min_qty_milli) {
    slice->qty_milli = bound.min_qty_milli;
  }
  if (slice->qty_milli > bound.max_qty_milli) {
    slice->qty_milli = bound.max_qty_milli;
  }
  return Status::kOk;
}

Status AllocationSolver::RedistributeDeficit(
    std::vector<ProRataSlice>* slices, std::uint32_t deficit_milli,
    std::uint32_t exclude_account) const {
  if (slices == nullptr || deficit_milli == 0) {
    return Status::kOk;
  }

  std::vector<std::size_t> donors;
  for (std::size_t i = 0; i < slices->size(); ++i) {
    if ((*slices)[i].account_id == exclude_account) {
      continue;
    }
    if ((*slices)[i].qty_milli > 0) {
      donors.push_back(i);
    }
  }

  if (donors.empty()) {
    return Status::kBoundsError;
  }

  std::uint32_t remaining = deficit_milli;
  std::size_t donor_idx = 0;
  std::uint32_t guard = 0;
  while (remaining > 0 && !donors.empty() && guard < deficit_milli * 4) {
    ProRataSlice& donor = (*slices)[donors[donor_idx % donors.size()]];
    if (donor.qty_milli > 0) {
      donor.qty_milli -= 1;
      remaining -= 1;
    }
    donor_idx += 1;
    guard += 1;
  }

  for (ProRataSlice& slice : *slices) {
    if (slice.account_id == exclude_account) {
      slice.qty_milli += deficit_milli - remaining;
      break;
    }
  }

  return remaining == 0 ? Status::kOk : Status::kBoundsError;
}

Status AllocationSolver::RedistributeSurplus(
    std::vector<ProRataSlice>* slices, std::uint32_t surplus_milli,
    std::uint32_t exclude_account) const {
  if (slices == nullptr || surplus_milli == 0) {
    return Status::kOk;
  }

  for (ProRataSlice& slice : *slices) {
    if (slice.account_id == exclude_account) {
      if (slice.qty_milli < surplus_milli) {
        return Status::kBoundsError;
      }
      slice.qty_milli -= surplus_milli;
      break;
    }
  }

  std::uint32_t remaining = surplus_milli;
  std::size_t idx = 0;
  std::uint32_t guard = 0;
  while (remaining > 0 && idx < slices->size() && guard < surplus_milli * 4) {
    if ((*slices)[idx].account_id != exclude_account) {
      (*slices)[idx].qty_milli += 1;
      remaining -= 1;
    }
    idx += 1;
    guard += 1;
  }

  return remaining == 0 ? Status::kOk : Status::kBoundsError;
}

bool AllocationSolver::CheckConvergence(
    const std::vector<ProRataSlice>& slices,
    const std::vector<AllocationBound>& bounds) const {
  return DetectViolations(slices, bounds).empty();
}

std::vector<AllocationBound> AllocationSolver::BuildDefaultBounds(
    const BatchWireFrame& frame) const {
  if (!custom_bounds_.empty()) {
    return custom_bounds_;
  }

  std::vector<AllocationBound> bounds;
  bounds.reserve(frame.records.size());
  for (const WireBatchRecord& rec : frame.records) {
    AllocationBound b{};
    b.account_id = rec.account_id;
    b.min_qty_milli = 0;
    b.max_qty_milli = rec.qty_milli * 2;
    bounds.push_back(b);
  }
  return bounds;
}

AllocationSolverResult AllocationSolver::SolveWithBounds(
    const std::vector<ProRataSlice>& initial,
    const std::vector<AllocationBound>& bounds,
    std::uint32_t total_qty_milli) {
  AllocationSolverResult result{};
  result.final_slices = initial;
  result.status = Status::kOk;

  if (!config_.enforce_bounds) {
    result.converged = true;
    return result;
  }

  for (std::uint32_t iter = 0; iter < config_.max_iterations; ++iter) {
    result.iterations_used = iter + 1;

    const std::vector<BoundViolation> violations =
        DetectViolations(result.final_slices, bounds);

    if (violations.empty()) {
      result.converged = true;
      break;
    }

    bool changed = false;
    for (const BoundViolation& v : violations) {
      for (ProRataSlice& slice : result.final_slices) {
        if (slice.account_id != v.account_id) {
          continue;
        }
        const AllocationBound* bound = FindBound(bounds, v.account_id);
        if (bound == nullptr) {
          break;
        }
        const std::uint32_t before = slice.qty_milli;
        ClampSlice(&slice, *bound);
        if (slice.qty_milli != before) {
          changed = true;
          result.bound_violations_fixed += 1;
        }
        break;
      }
    }

    const std::uint32_t current_total = SumSliceQty(result.final_slices);
    if (current_total < total_qty_milli) {
      const std::uint32_t deficit = total_qty_milli - current_total;
      for (const BoundViolation& v : violations) {
        if (v.deficit > 0) {
          RedistributeDeficit(&result.final_slices,
                              static_cast<std::uint32_t>(v.deficit),
                              v.account_id);
          changed = true;
          break;
        }
      }
      if (!changed) {
        RedistributeDeficit(&result.final_slices, deficit, 0);
      }
    } else if (current_total > total_qty_milli) {
      const std::uint32_t surplus = current_total - total_qty_milli;
      for (const BoundViolation& v : violations) {
        if (v.surplus > 0) {
          RedistributeSurplus(&result.final_slices,
                              static_cast<std::uint32_t>(v.surplus),
                              v.account_id);
          changed = true;
          break;
        }
      }
      if (!changed) {
        RedistributeSurplus(&result.final_slices, surplus, 0);
      }
    }

    if (!changed) {
      break;
    }
  }

  const std::uint32_t final_total = SumSliceQty(result.final_slices);
  if (final_total != total_qty_milli) {
    result.converged = false;
  } else {
    result.converged = CheckConvergence(result.final_slices, bounds);
  }

  return result;
}

AllocationSolverResult AllocationSolver::Solve(
    const BatchWireFrame& frame, const std::vector<ProRataSlice>& initial) {
  std::uint32_t total_qty = 0;
  for (const WireBatchRecord& rec : frame.records) {
    total_qty += rec.qty_milli;
  }

  const std::vector<AllocationBound> bounds = BuildDefaultBounds(frame);
  return SolveWithBounds(initial, bounds, total_qty);
}

}  // namespace desk
}  // namespace tkr
