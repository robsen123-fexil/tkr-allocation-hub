#include "tkr/desk/pro_rata_allocator.h"

#include <algorithm>
#include <vector>

namespace tkr {
namespace desk {
namespace {

struct FractionalRank {
  std::uint32_t account_id;
  std::uint32_t fractional_remainder;
  std::size_t original_index;
};

std::uint32_t SumWeightBp(const std::vector<ProRataWeight>& weights) {
  std::uint32_t total = 0;
  for (const ProRataWeight& w : weights) {
    total += w.weight_bp;
  }
  return total;
}

}  // namespace

ProRataAllocator::ProRataAllocator(ProRataAllocatorConfig config)
    : config_(config) {}

std::uint32_t ProRataAllocator::ComputeFloorShare(std::uint32_t total_qty,
                                                  std::uint32_t weight_bp,
                                                  std::uint32_t total_weight_bp) {
  if (total_weight_bp == 0 || weight_bp == 0) {
    return 0;
  }
  const std::uint64_t product =
      static_cast<std::uint64_t>(total_qty) * static_cast<std::uint64_t>(weight_bp);
  return static_cast<std::uint32_t>(product / total_weight_bp);
}

std::uint32_t ProRataAllocator::ComputeFractionalRemainder(
    std::uint32_t total_qty, std::uint32_t weight_bp,
    std::uint32_t total_weight_bp) {
  if (total_weight_bp == 0 || weight_bp == 0) {
    return 0;
  }
  const std::uint64_t product =
      static_cast<std::uint64_t>(total_qty) * static_cast<std::uint64_t>(weight_bp);
  return static_cast<std::uint32_t>(product % total_weight_bp);
}

Status ProRataAllocator::ValidateWeights(
    const std::vector<ProRataWeight>& weights) const {
  if (weights.empty()) {
    return Status::kBoundsError;
  }
  std::uint32_t total_bp = SumWeightBp(weights);
  if (total_bp == 0) {
    return Status::kBoundsError;
  }
  for (const ProRataWeight& w : weights) {
    if (w.weight_bp == 0) {
      return Status::kBoundsError;
    }
  }
  return Status::kOk;
}

std::vector<ProRataWeight> ProRataAllocator::ExtractWeightsFromFrame(
    const BatchWireFrame& frame) const {
  std::vector<ProRataWeight> weights;
  weights.reserve(frame.records.size());
  for (const WireBatchRecord& rec : frame.records) {
    ProRataWeight w{};
    w.account_id = rec.account_id;
    w.weight_bp = rec.qty_milli;
    weights.push_back(w);
  }
  return weights;
}

void ProRataAllocator::SortByFractionalRemainder(
    std::vector<ProRataSlice>* slices, const std::vector<ProRataWeight>& weights,
    std::uint32_t total_qty_milli, std::uint32_t total_weight_bp) const {
  if (slices == nullptr || !config_.distribute_remainder_by_largest_fraction) {
    return;
  }

  std::vector<FractionalRank> ranks;
  ranks.reserve(slices->size());
  for (std::size_t i = 0; i < slices->size(); ++i) {
    FractionalRank rank{};
    rank.account_id = (*slices)[i].account_id;
    rank.original_index = i;
    for (const ProRataWeight& w : weights) {
      if (w.account_id == rank.account_id) {
        rank.fractional_remainder =
            ComputeFractionalRemainder(total_qty_milli, w.weight_bp, total_weight_bp);
        break;
      }
    }
    ranks.push_back(rank);
  }

  std::sort(ranks.begin(), ranks.end(),
            [](const FractionalRank& a, const FractionalRank& b) {
              if (a.fractional_remainder != b.fractional_remainder) {
                return a.fractional_remainder > b.fractional_remainder;
              }
              return a.account_id < b.account_id;
            });

  for (std::size_t i = 0; i < ranks.size(); ++i) {
    for (ProRataSlice& slice : *slices) {
      if (slice.account_id == ranks[i].account_id) {
        slice.remainder_rank = static_cast<std::uint32_t>(i);
        break;
      }
    }
  }
}

ProRataAllocatorResult ProRataAllocator::AllocateWithWeights(
    std::uint32_t total_qty_milli, const std::vector<ProRataWeight>& weights,
    const std::vector<WireBatchRecord>& records) {
  ProRataAllocatorResult result{};
  result.status = ValidateWeights(weights);
  if (result.status != Status::kOk) {
    return result;
  }

  const std::uint32_t total_weight_bp = SumWeightBp(weights);
  result.slices.reserve(weights.size());

  std::uint32_t allocated = 0;
  for (std::size_t i = 0; i < weights.size(); ++i) {
    const ProRataWeight& w = weights[i];
    ProRataSlice slice{};
    slice.account_id = w.account_id;
    slice.record_id = (i < records.size()) ? records[i].record_id : 0;
    slice.qty_milli =
        ComputeFloorShare(total_qty_milli, w.weight_bp, total_weight_bp);
    slice.remainder_rank = 0;
    allocated += slice.qty_milli;
    result.slices.push_back(slice);
  }

  SortByFractionalRemainder(&result.slices, weights, total_qty_milli,
                            total_weight_bp);

  std::uint32_t remainder = total_qty_milli - allocated;
  result.remainder_units = remainder;

  if (config_.distribute_remainder_by_largest_fraction && remainder > 0) {
    std::vector<ProRataSlice> ranked = result.slices;
    std::sort(ranked.begin(), ranked.end(),
              [](const ProRataSlice& a, const ProRataSlice& b) {
                if (a.remainder_rank != b.remainder_rank) {
                  return a.remainder_rank < b.remainder_rank;
                }
                return a.account_id < b.account_id;
              });

    for (std::uint32_t r = 0; r < remainder && r < ranked.size(); ++r) {
      for (ProRataSlice& slice : result.slices) {
        if (slice.account_id == ranked[r].account_id) {
          slice.qty_milli += 1;
          break;
        }
      }
    }
    result.total_allocated_milli = total_qty_milli;
  } else {
    result.total_allocated_milli = allocated;
  }

  result.status = Status::kOk;
  return result;
}

namespace {

struct AllocationSummaryRow {
  std::uint32_t account_id;
  std::uint32_t weight_bp;
  std::uint32_t raw_floor;
  std::uint32_t fractional_part;
  std::uint32_t final_qty;
};

std::vector<AllocationSummaryRow> BuildSummaryTable(
    std::uint32_t total_qty, const std::vector<ProRataWeight>& weights,
    const std::vector<ProRataSlice>& slices) {
  const std::uint32_t total_weight = SumWeightBp(weights);
  std::vector<AllocationSummaryRow> rows;
  rows.reserve(weights.size());

  for (const ProRataWeight& w : weights) {
    AllocationSummaryRow row{};
    row.account_id = w.account_id;
    row.weight_bp = w.weight_bp;
    row.raw_floor =
        ProRataAllocator::ComputeFloorShare(total_qty, w.weight_bp, total_weight);
    row.fractional_part = ProRataAllocator::ComputeFractionalRemainder(
        total_qty, w.weight_bp, total_weight);
    row.final_qty = row.raw_floor;
    for (const ProRataSlice& slice : slices) {
      if (slice.account_id == w.account_id) {
        row.final_qty = slice.qty_milli;
        break;
      }
    }
    rows.push_back(row);
  }
  return rows;
}

bool VerifyTotalConservation(const std::vector<ProRataSlice>& slices,
                             std::uint32_t expected_total) {
  std::uint32_t sum = 0;
  for (const ProRataSlice& slice : slices) {
    sum += slice.qty_milli;
  }
  return sum == expected_total;
}

}  // namespace

ProRataAllocatorResult ProRataAllocator::Allocate(const BatchWireFrame& frame) {
  ProRataAllocatorResult result{};

  if (frame.records.empty()) {
    result.status = Status::kBoundsError;
    return result;
  }

  std::uint32_t total_qty = 0;
  for (const WireBatchRecord& rec : frame.records) {
    total_qty += rec.qty_milli;
  }

  if ((frame.header.flags & kBatchFlagProRata) != 0) {
    std::vector<ProRataWeight> weights = ExtractWeightsFromFrame(frame);
    result = AllocateWithWeights(total_qty, weights, frame.records);
    if (result.status == Status::kOk &&
        !VerifyTotalConservation(result.slices, total_qty)) {
      result.status = Status::kBoundsError;
    }
    return result;
  }

  std::vector<ProRataWeight> equal_weights;
  equal_weights.reserve(frame.records.size());
  const std::uint32_t equal_bp =
      10000u / static_cast<std::uint32_t>(frame.records.size());
  std::uint32_t remainder_bp =
      10000u - (equal_bp * static_cast<std::uint32_t>(frame.records.size()));

  for (std::size_t i = 0; i < frame.records.size(); ++i) {
    ProRataWeight w{};
    w.account_id = frame.records[i].account_id;
    w.weight_bp = equal_bp + ((i < remainder_bp) ? 1u : 0u);
    equal_weights.push_back(w);
  }

  result = AllocateWithWeights(total_qty, equal_weights, frame.records);
  return result;
}

}  // namespace desk
}  // namespace tkr
