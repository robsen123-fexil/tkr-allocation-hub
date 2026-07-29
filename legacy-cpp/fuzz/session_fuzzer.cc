#include "tkr/engine/pipeline_orchestrator.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  tkr::engine::MergeSessionLegs(data, size);
  return 0;
}
