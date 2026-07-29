#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

struct ProRataWeight {
  std::uint32_t account_id;
  std::uint32_t weight_bp;  // basis points, sum to 10000
};

struct ProRataSlice {
  std::uint32_t account_id;
  std::uint32_t record_id;
  std::uint32_t qty_milli;
  std::uint32_t remainder_rank;
};

struct ProRataAllocatorConfig {
  bool distribute_remainder_by_largest_fraction;
};

struct ProRataAllocatorResult {
  Status status;
  std::vector<ProRataSlice> slices;
  std::uint32_t total_allocated_milli;
  std::uint32_t remainder_units;
};

class ProRataAllocator {
 public:
  explicit ProRataAllocator(ProRataAllocatorConfig config);

  ProRataAllocatorResult Allocate(const BatchWireFrame& frame);
  ProRataAllocatorResult AllocateWithWeights(
      std::uint32_t total_qty_milli,
      const std::vector<ProRataWeight>& weights,
      const std::vector<WireBatchRecord>& records);

  static std::uint32_t ComputeFloorShare(std::uint32_t total_qty,
                                         std::uint32_t weight_bp,
                                         std::uint32_t total_weight_bp);
  static std::uint32_t ComputeFractionalRemainder(std::uint32_t total_qty,
                                                  std::uint32_t weight_bp,
                                                  std::uint32_t total_weight_bp);

 private:
  Status ValidateWeights(const std::vector<ProRataWeight>& weights) const;
  void SortByFractionalRemainder(std::vector<ProRataSlice>* slices,
                                 const std::vector<ProRataWeight>& weights,
                                 std::uint32_t total_qty_milli,
                                 std::uint32_t total_weight_bp) const;
  std::vector<ProRataWeight> ExtractWeightsFromFrame(
      const BatchWireFrame& frame) const;

  ProRataAllocatorConfig config_;
};

}  // namespace desk
}  // namespace tkr
