#include "tkr/ledger/channel_tape.h"

#include "tkr/util/bounds.h"

#include <algorithm>

namespace tkr {
namespace ledger {
namespace {

struct TapeDigestEntry {
  std::uint32_t envelope_seq;
  std::uint32_t channel_id;
  std::uint32_t payload_digest;
  std::uint32_t payload_len;
};

}  // namespace

ChannelTape& GlobalChannelTape() {
  static ChannelTape tape;
  return tape;
}

void ChannelTape::QueueDeferredChannel(std::uint32_t envelope_seq,
                                       const ChannelView& view) {
  ChannelTapeEntry entry{};
  entry.envelope_seq = envelope_seq;
  entry.channel_id = view.channel_id;
  entry.payload_ptr = view.payload_ptr;
  entry.payload_len = view.payload_len;
  entry.sealed = false;
  entries_.push_back(entry);
}

void ChannelTape::ClearPending() {
  entries_.clear();
  next_seal_round_ = 0;
}

SealEnvelopeResult ChannelTape::SealDeferredEnvelope() {
  SealEnvelopeResult result{};
  result.status = Status::kOk;
  std::uint32_t seal = 2166136261u;

  std::vector<TapeDigestEntry> digest_entries;
  digest_entries.reserve(entries_.size());

  for (ChannelTapeEntry& entry : entries_) {
    if (entry.payload_ptr == nullptr || entry.payload_len == 0) {
      continue;
    }

    TapeDigestEntry de{};
    de.envelope_seq = entry.envelope_seq;
    de.channel_id = entry.channel_id;
    de.payload_len = entry.payload_len;
    de.payload_digest = util::Fnv1a32(entry.payload_ptr, entry.payload_len);
    digest_entries.push_back(de);
  }

  std::sort(digest_entries.begin(), digest_entries.end(),
            [](const TapeDigestEntry& a, const TapeDigestEntry& b) {
              if (a.envelope_seq != b.envelope_seq) {
                return a.envelope_seq < b.envelope_seq;
              }
              return a.channel_id < b.channel_id;
            });

  for (const TapeDigestEntry& de : digest_entries) {
    seal ^= de.payload_digest;
    seal *= 16777619u;
    seal ^= de.channel_id;
    seal ^= de.envelope_seq;
    seal ^= de.payload_len;
  }

  for (ChannelTapeEntry& entry : entries_) {
    if (entry.payload_ptr == nullptr || entry.payload_len == 0) {
      continue;
    }
    entry.sealed = true;
    ++result.channels_sealed;
  }

  result.seal_digest = seal;
  ++next_seal_round_;
  return result;
}

}  // namespace ledger
}  // namespace tkr
