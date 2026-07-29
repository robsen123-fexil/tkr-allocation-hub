#pragma once

#include "tkr/types.h"

#include <vector>

namespace tkr {
namespace ledger {

struct ChannelTapeEntry {
  std::uint32_t envelope_seq;
  std::uint32_t channel_id;
  const std::uint8_t* payload_ptr;
  std::uint32_t payload_len;
  bool sealed;
};

struct SealEnvelopeResult {
  Status status;
  std::uint32_t seal_digest;
  std::uint32_t channels_sealed;
};

class ChannelTape {
 public:
  void QueueDeferredChannel(std::uint32_t envelope_seq, const ChannelView& view);
  void ClearPending();
  SealEnvelopeResult SealDeferredEnvelope();

  const std::vector<ChannelTapeEntry>& Entries() const { return entries_; }

 private:
  std::vector<ChannelTapeEntry> entries_;
  std::uint32_t next_seal_round_;
};

ChannelTape& GlobalChannelTape();

}  // namespace ledger
}  // namespace tkr
