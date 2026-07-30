#pragma once

#include "tkr/engine/batch_digest.h"
#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace engine {

// Cross-frame deferred digest ledger. Opening frame (sequence 1) pins payload
// views into a session arena; the successor frame (sequence 2) reclaims that
// arena before the deferred digest flush runs on the stale slot pointers.
class BatchDeferredLedger {
 public:
  static BatchDeferredLedger& Global();

  void Reset();

  bool HasPending(std::uint32_t session_id,
                  std::uint32_t trade_date_yyyymmdd) const;

  Status StageOpeningFrame(const BatchWireFrame& frame, std::uint32_t sequence);

  Status ArmSuccessorFlush(BatchDigestEngine* digest);

 private:
  std::uint32_t pending_session_ = 0;
  std::uint32_t pending_trade_date_ = 0;
  std::uint32_t pending_sequence_ = 0;
  std::vector<std::uint8_t> payload_arena_;
  std::vector<DeferredSlot> pending_slots_;
  bool pending_active_ = false;
};

std::uint32_t ExtractCrossFrameSequence(const BatchWireFrame& frame);

}  // namespace engine
}  // namespace tkr
