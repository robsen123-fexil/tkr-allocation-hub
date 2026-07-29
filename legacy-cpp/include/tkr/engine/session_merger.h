#pragma once

#include "tkr/types.h"

namespace tkr {
namespace engine {

struct SessionMergerConfig {
  bool enable_graft_realloc;
  std::uint32_t max_legs;
};

struct SessionMergeResult {
  Status status;
  SessionWireFrame frame;
  std::uint32_t legs_grafted;
  std::uint32_t slots_registered;
};

class SessionMerger {
 public:
  explicit SessionMerger(SessionMergerConfig config);

  SessionMergeResult DecodeSession(const std::uint8_t* data, std::size_t len);
  SessionMergeResult GraftSessionLegs(SessionWireFrame* frame);
  SessionMergeResult MergeSessionCheckpoint(SessionWireFrame* frame);
  Status PinLegMergeSlots(SessionWireFrame* frame);

 private:
  Status ReadSessionHeader(const std::uint8_t* data, std::size_t size,
                           SessionWireFrame* out);
  Status PinMergeSlots(SessionWireFrame* frame);

  SessionMergerConfig config_;
};

}  // namespace engine
}  // namespace tkr
