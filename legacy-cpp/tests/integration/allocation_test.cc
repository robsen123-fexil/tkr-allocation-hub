#include "tkr/desk/allocation_solver.h"
#include "tkr/desk/pro_rata_allocator.h"
#include "tkr/types.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int Fail(const char* msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return 1;
}

int TestProRata1000Shares() {
  // 1000 shares with 40/30/30 weights -> 400/300/300
  std::vector<tkr::desk::ProRataWeight> weights;
  std::vector<tkr::WireBatchRecord> records;

  const std::uint32_t accounts[] = {101, 102, 103};
  const std::uint32_t weight_vals[] = {4000, 3000, 3000};

  for (std::size_t i = 0; i < 3; ++i) {
    tkr::desk::ProRataWeight w{};
    w.account_id = accounts[i];
    w.weight_bp = weight_vals[i];
    weights.push_back(w);

    tkr::WireBatchRecord rec{};
    rec.record_id = static_cast<std::uint32_t>(i + 1);
    rec.account_id = accounts[i];
    rec.qty_milli = weight_vals[i];
    records.push_back(rec);
  }

  tkr::desk::ProRataAllocator allocator(
      tkr::desk::ProRataAllocatorConfig{true});
  tkr::desk::ProRataAllocatorResult result =
      allocator.AllocateWithWeights(1000, weights, records);

  if (result.status != tkr::Status::kOk) {
    return Fail("pro-rata allocation returned non-ok status");
  }

  if (result.slices.size() != 3) {
    return Fail("expected 3 slices");
  }

  const std::uint32_t expected[] = {400, 300, 300};
  for (std::size_t i = 0; i < 3; ++i) {
    bool found = false;
    for (const tkr::desk::ProRataSlice& slice : result.slices) {
      if (slice.account_id == accounts[i]) {
        if (slice.qty_milli != expected[i]) {
          std::cerr << "account " << accounts[i] << " got " << slice.qty_milli
                    << " expected " << expected[i] << "\n";
          return Fail("incorrect pro-rata share");
        }
        found = true;
        break;
      }
    }
    if (!found) {
      return Fail("missing account in slices");
    }
  }

  if (result.total_allocated_milli != 1000) {
    return Fail("total allocated != 1000");
  }

  return 0;
}

int TestAllocationSolverBounds() {
  std::vector<tkr::desk::ProRataSlice> slices;
  tkr::desk::ProRataSlice s1{};
  s1.account_id = 101;
  s1.qty_milli = 600;
  slices.push_back(s1);

  tkr::desk::ProRataSlice s2{};
  s2.account_id = 102;
  s2.qty_milli = 400;
  slices.push_back(s2);

  std::vector<tkr::desk::AllocationBound> bounds;
  tkr::desk::AllocationBound b1{};
  b1.account_id = 101;
  b1.min_qty_milli = 0;
  b1.max_qty_milli = 500;
  bounds.push_back(b1);

  tkr::desk::AllocationBound b2{};
  b2.account_id = 102;
  b2.min_qty_milli = 0;
  b2.max_qty_milli = 600;
  bounds.push_back(b2);

  tkr::desk::AllocationSolver solver(
      tkr::desk::AllocationSolverConfig{true, 100});
  tkr::desk::AllocationSolverResult result =
      solver.SolveWithBounds(slices, bounds, 1000);

  if (result.status != tkr::Status::kOk && !result.converged) {
    return Fail("solver did not converge");
  }

  for (const tkr::desk::ProRataSlice& slice : result.final_slices) {
    if (slice.account_id == 101 && slice.qty_milli > 500) {
      return Fail("account 101 exceeds max bound");
    }
  }

  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += TestProRata1000Shares();
  failures += TestAllocationSolverBounds();

  if (failures == 0) {
    std::cout << "All allocation tests passed.\n";
    return 0;
  }

  std::cerr << failures << " test(s) failed.\n";
  return 1;
}
