#pragma once

#include "tkr/types.h"

#include <cstddef>
#include <cstdint>

namespace tkr {
namespace engine {

struct PipelineStats {
  std::uint32_t batches_processed;
  std::uint32_t envelopes_sealed;
  std::uint32_t sessions_merged;
  std::uint32_t desk_calls;
};

Status RunAllocationPipeline(const std::uint8_t* data, std::size_t len);
Status RunRouterPipeline(const std::uint8_t* data, std::size_t len);
Status MergeSessionLegs(const std::uint8_t* data, std::size_t len);

const PipelineStats& LastPipelineStats();

}  // namespace engine
}  // namespace tkr
