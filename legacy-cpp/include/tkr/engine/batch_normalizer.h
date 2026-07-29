#pragma once

#include "tkr/types.h"

namespace tkr {
namespace engine {

struct BatchNormalizeConfig {
  bool compact_payloads;
  bool rewrite_record_offsets;
  bool apply_pro_rata_hints;
};

struct BatchNormalizeResult {
  Status status;
  BatchWireFrame frame;
  std::uint32_t records_rewritten;
  std::uint32_t bytes_compacted;
};

class BatchNormalizer {
 public:
  explicit BatchNormalizer(BatchNormalizeConfig config);

  BatchNormalizeResult NormalizeBatchRecords(BatchWireFrame* frame);

 private:
  Status RewritePayloadLayout(BatchWireFrame* frame);
  Status ApplyDeskHints(BatchWireFrame* frame);

  BatchNormalizeConfig config_;
};

}  // namespace engine
}  // namespace tkr
