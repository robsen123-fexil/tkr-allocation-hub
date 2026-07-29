#pragma once

#include "tkr/types.h"

namespace tkr {
namespace desk {

struct DeskRunContext {
  std::uint32_t batch_id;
  std::uint32_t record_count;
  std::uint32_t total_qty_milli;
  bool margin_ok;
  bool compliance_ok;
};

class DeskRegistry {
 public:
  Status RunBatchDesks(BatchWireFrame& frame, DeskRunContext* ctx);
  Status RunCompliancePass(const BatchWireFrame& frame);
  Status RunMarginPass(const BatchWireFrame& frame);
  Status RunHaircutPass(const BatchWireFrame& frame);
  Status RunCorporateActionPass(const BatchWireFrame& frame);
};

}  // namespace desk
}  // namespace tkr
