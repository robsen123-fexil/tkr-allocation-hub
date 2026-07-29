#pragma once

#include "tkr/types.h"

#include <vector>

namespace tkr {
namespace engine {

struct MergeDigestResult {
  Status status;
  std::uint32_t digest;
  std::uint32_t slots_hashed;
};

class MergeDigestEngine {
 public:
  void RegisterMergeSlots(const std::vector<MergeSlot>& slots);
  MergeDigestResult FlushMergeDigest();

 private:
  std::vector<MergeSlot> pinned_slots_;
};

}  // namespace engine
}  // namespace tkr
