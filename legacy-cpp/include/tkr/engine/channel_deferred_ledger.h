#pragma once

#include "tkr/ledger/channel_tape.h"
#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace engine {

// Cross-frame channel tape ledger. An opening TKR2 envelope (sequence 1) pins
// channel payload views into a session arena; a TKR1 batch mutation frame must
// arrive before a successor envelope (sequence 2) reclaims the arena and seals
// through the stale deferred channel pointers.
class ChannelDeferredLedger {
 public:
  static ChannelDeferredLedger& Global();

  void Reset();

  bool HasPending(std::uint32_t session_id) const;
  bool ReadyForSuccessor(std::uint32_t session_id) const;

  Status StageOpeningEnvelope(std::uint32_t session_id,
                              std::uint32_t envelope_seq,
                              const std::vector<ChannelView>& views,
                              const std::vector<std::uint8_t>& payload);

  Status ApplyMutation(std::uint32_t session_id);

  Status ArmSuccessorSeal(ledger::ChannelTape* tape);

 private:
  std::uint32_t pending_session_ = 0;
  std::uint32_t pending_envelope_seq_ = 0;
  std::vector<std::uint8_t> payload_arena_;
  std::vector<ledger::ChannelTapeEntry> pending_entries_;
  bool pending_active_ = false;
  bool mutation_applied_ = false;
};

std::uint32_t ExtractEnvelopeCrossFrameSequence(
    const std::vector<WireEnvelopeChannel>& channels);

Status BuildCrossFrameChannelViews(const WireEnvelope& wire,
                                   const std::vector<std::uint8_t>& payload,
                                   std::vector<ChannelView>* views);

Status DecodeRouterEnvelopeFrame(const std::uint8_t* data, std::size_t size,
                                 WireEnvelope* wire,
                                 std::vector<std::uint8_t>* payload,
                                 std::size_t* consumed_bytes);

}  // namespace engine
}  // namespace tkr
