#pragma once

#include "tkr/desk/pro_rata_allocator.h"
#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

struct AllocationBound {
  std::uint32_t account_id;
  std::uint32_t min_qty_milli;
  std::uint32_t max_qty_milli;
};

struct AllocationSolverConfig {
  bool enforce_bounds;
  std::uint32_t max_iterations;
};

struct AllocationSolverResult {
  Status status;
  std::vector<ProRataSlice> final_slices;
  std::uint32_t iterations_used;
  std::uint32_t bound_violations_fixed;
  bool converged;
};

class AllocationSolver {
 public:
  explicit AllocationSolver(AllocationSolverConfig config);

  AllocationSolverResult Solve(const BatchWireFrame& frame,
                               const std::vector<ProRataSlice>& initial);
  AllocationSolverResult SolveWithBounds(
      const std::vector<ProRataSlice>& initial,
      const std::vector<AllocationBound>& bounds,
      std::uint32_t total_qty_milli);

  void SetBound(std::uint32_t account_id, std::uint32_t min_qty,
                std::uint32_t max_qty);
  void ClearBounds();

 private:
  Status ClampSlice(ProRataSlice* slice, const AllocationBound& bound) const;
  Status RedistributeDeficit(std::vector<ProRataSlice>* slices,
                             std::uint32_t deficit_milli,
                             std::uint32_t exclude_account) const;
  Status RedistributeSurplus(std::vector<ProRataSlice>* slices,
                             std::uint32_t surplus_milli,
                             std::uint32_t exclude_account) const;
  bool CheckConvergence(const std::vector<ProRataSlice>& slices,
                        const std::vector<AllocationBound>& bounds) const;
  std::vector<AllocationBound> BuildDefaultBounds(
      const BatchWireFrame& frame) const;

  AllocationSolverConfig config_;
  std::vector<AllocationBound> custom_bounds_;
};

}  // namespace desk
}  // namespace tkr
