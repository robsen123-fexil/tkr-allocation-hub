#include "tkr/engine/merge_digest.h"

#include "tkr/util/bounds.h"

#include <algorithm>

namespace tkr {
namespace engine {
namespace {

constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

struct MergeDigestEntry {
  std::uint32_t slot_id;
  std::uint32_t leg_id;
  std::uint32_t checkpoint_seq;
  std::uint32_t chunk_digest;
  std::uint32_t ref_len;
  bool pinned;
};

std::uint32_t MixDigest(std::uint32_t hash, std::uint32_t value) {
  hash ^= value;
  hash *= kFnvPrime;
  return hash;
}

}  // namespace

void MergeDigestEngine::RegisterMergeSlots(
    const std::vector<MergeSlot>& slots) {
  pinned_slots_ = slots;
}

MergeDigestResult MergeDigestEngine::FlushMergeDigest() {
  MergeDigestResult result{};
  result.status = Status::kOk;
  std::uint32_t digest = kFnvOffset;

  std::vector<MergeDigestEntry> entries;
  entries.reserve(pinned_slots_.size());

  for (const MergeSlot& slot : pinned_slots_) {
    if (!slot.pinned) {
      continue;
    }

    MergeDigestEntry entry{};
    entry.slot_id = slot.slot_id;
    entry.leg_id = slot.leg_id;
    entry.checkpoint_seq = slot.checkpoint_seq;
    entry.ref_len = slot.ref_len;
    entry.pinned = slot.pinned;

    if (slot.ref_ptr != nullptr && slot.ref_len > 0) {
      entry.chunk_digest = util::Fnv1a32(slot.ref_ptr, slot.ref_len);
    }

    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(),
            [](const MergeDigestEntry& a, const MergeDigestEntry& b) {
              if (a.checkpoint_seq != b.checkpoint_seq) {
                return a.checkpoint_seq < b.checkpoint_seq;
              }
              if (a.leg_id != b.leg_id) {
                return a.leg_id < b.leg_id;
              }
              return a.slot_id < b.slot_id;
            });

  for (const MergeDigestEntry& entry : entries) {
    digest = MixDigest(digest, entry.chunk_digest);
    digest = MixDigest(digest, entry.leg_id);
    digest = MixDigest(digest, entry.slot_id);
    digest = MixDigest(digest, entry.checkpoint_seq);
    digest = MixDigest(digest, entry.ref_len);
    ++result.slots_hashed;
  }

  digest = MixDigest(digest, result.slots_hashed);
  result.digest = digest;
  return result;
}

}  // namespace engine
}  // namespace tkr
