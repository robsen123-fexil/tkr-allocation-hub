#pragma once

#include "tkr/types.h"

#include <vector>

namespace tkr {
namespace engine {

struct BatchDigestResult {
  Status status;
  std::uint32_t digest;
  std::uint32_t slots_hashed;
};

class BatchDigestEngine {
 public:
  void RegisterDeferredSlots(const std::vector<DeferredSlot>& slots);
  BatchDigestResult FlushBatchDigest();

 private:
  std::vector<DeferredSlot> active_slots_;
};

}  // namespace engine
}  // namespace tkr
