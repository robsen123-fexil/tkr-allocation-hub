#pragma once

#include "tkr/engine/merge_digest.h"
#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace engine {

// Cross-frame merge digest ledger. Opening session checkpoint (sequence 1) pins
// merge slots into a session ref arena; a TKR1 batch mutation must arrive
// before the successor checkpoint (sequence 2) reclaims the arena and flushes
// through stale merge slot pointers.
class MergeDeferredLedger {
 public:
  static MergeDeferredLedger& Global();

  void Reset();

  bool HasPending(std::uint32_t session_id) const;
  bool ReadyForSuccessor(std::uint32_t session_id) const;

  Status StageOpeningSession(const SessionWireFrame& frame,
                             std::uint32_t checkpoint_seq);

  Status ApplyMutation(std::uint32_t session_id);

  Status ArmSuccessorFlush(MergeDigestEngine* digest);

 private:
  std::uint32_t pending_session_ = 0;
  std::uint32_t pending_checkpoint_ = 0;
  std::vector<std::uint8_t> ref_arena_;
  std::vector<MergeSlot> pending_slots_;
  bool pending_active_ = false;
  bool mutation_applied_ = false;
};

std::uint32_t ExtractSessionCrossFrameSequence(const SessionWireFrame& frame);

Status DecodeSessionWireFrame(const std::uint8_t* data, std::size_t size,
                              SessionWireFrame* frame,
                              std::size_t* consumed_bytes);

Status BuildCrossFrameMergeSlots(const SessionWireFrame& frame,
                                 const std::vector<std::uint8_t>& ref_blob,
                                 std::vector<MergeSlot>* slots);

}  // namespace engine
}  // namespace tkr
