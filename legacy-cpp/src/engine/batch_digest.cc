#include "tkr/engine/batch_digest.h"

#include "tkr/util/bounds.h"

#include <algorithm>

namespace tkr {
namespace engine {
namespace {

constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

struct SlotDigestEntry {
  std::uint32_t slot_id;
  std::uint32_t record_id;
  std::uint32_t chunk_digest;
  std::uint32_t payload_len;
  bool active;
};

std::uint32_t MixDigest(std::uint32_t hash, std::uint32_t value) {
  hash ^= value;
  hash *= kFnvPrime;
  return hash;
}

bool ValidateSlotBounds(const DeferredSlot& slot) {
  if (!slot.active) {
    return true;
  }
  if (slot.payload_ptr == nullptr && slot.payload_len > 0) {
    return false;
  }
  return true;
}

}  // namespace

void BatchDigestEngine::RegisterDeferredSlots(
    const std::vector<DeferredSlot>& slots) {
  active_slots_ = slots;
}

BatchDigestResult BatchDigestEngine::FlushBatchDigest() {
  BatchDigestResult result{};
  result.status = Status::kOk;
  std::uint32_t digest = kFnvOffset;

  std::vector<SlotDigestEntry> entries;
  entries.reserve(active_slots_.size());

  for (const DeferredSlot& slot : active_slots_) {
    if (!ValidateSlotBounds(slot)) {
      result.status = Status::kBoundsError;
      return result;
    }

    if (!slot.active || slot.payload_ptr == nullptr || slot.payload_len == 0) {
      continue;
    }

    SlotDigestEntry entry{};
    entry.slot_id = slot.slot_id;
    entry.record_id = slot.record_id;
    entry.payload_len = slot.payload_len;
    entry.active = slot.active;
    entry.chunk_digest = util::Fnv1a32(slot.payload_ptr, slot.payload_len);
    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(),
            [](const SlotDigestEntry& a, const SlotDigestEntry& b) {
              if (a.record_id != b.record_id) {
                return a.record_id < b.record_id;
              }
              return a.slot_id < b.slot_id;
            });

  for (const SlotDigestEntry& entry : entries) {
    digest = MixDigest(digest, entry.chunk_digest);
    digest = MixDigest(digest, entry.record_id);
    digest = MixDigest(digest, entry.slot_id);
    digest = MixDigest(digest, entry.payload_len);
    ++result.slots_hashed;
  }

  digest = MixDigest(digest, result.slots_hashed);
  result.digest = digest;
  return result;
}

}  // namespace engine
}  // namespace tkr
