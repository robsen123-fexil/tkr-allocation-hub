#include "tkr/engine/batch_normalizer.h"

#include "tkr/util/bounds.h"

#include <algorithm>
#include <vector>

namespace tkr {
namespace engine {
namespace {

struct RecordPayloadView {
  std::uint32_t record_id;
  std::uint32_t old_offset;
  std::uint32_t new_offset;
  std::uint32_t payload_len;
};

bool RecordsSortedByOffset(const BatchWireFrame& frame) {
  std::uint32_t prev = 0;
  for (const WireBatchRecord& rec : frame.records) {
    if (rec.payload_len == 0) {
      continue;
    }
    if (rec.payload_offset < prev) {
      return false;
    }
    prev = rec.payload_offset;
  }
  return true;
}

std::uint32_t ComputeRecordFlagsHint(const WireBatchRecord& rec,
                                     std::uint32_t batch_flags) {
  std::uint32_t hint = rec.flags;
  if ((batch_flags & kBatchFlagProRata) != 0) {
    hint |= kBatchFlagProRata;
  }
  if ((batch_flags & kBatchFlagMarginCheck) != 0) {
    hint |= kBatchFlagMarginCheck;
  }
  return hint;
}

}  // namespace

BatchNormalizer::BatchNormalizer(BatchNormalizeConfig config) : config_(config) {}

Status BatchNormalizer::ApplyDeskHints(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }

  for (WireBatchRecord& rec : frame->records) {
    rec.flags = ComputeRecordFlagsHint(rec, frame->header.flags);

    if ((rec.flags & kBatchFlagProRata) != 0 && rec.qty_milli > 0) {
      rec.price_tick = rec.price_tick == 0 ? 100 : rec.price_tick;
    }
    if ((rec.flags & kBatchFlagComplianceHold) != 0) {
      rec.flags |= kBatchFlagMarginCheck;
    }
    if ((frame->header.flags & kBatchFlagCrossDesk) != 0) {
      rec.flags |= kBatchFlagCrossDesk;
    }
  }

  return Status::kOk;
}

Status BatchNormalizer::RewritePayloadLayout(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }
  if (!config_.compact_payloads || frame->payload_blob.empty()) {
    return Status::kOk;
  }

  if ((frame->header.flags & kBatchFlagDeferredDigest) == 0 &&
      RecordsSortedByOffset(*frame) && frame->records.size() <= 1) {
    return Status::kOk;
  }

  std::vector<RecordPayloadView> views;
  views.reserve(frame->records.size());

  for (const WireBatchRecord& rec : frame->records) {
    if (rec.payload_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(rec.payload_offset, rec.payload_len,
                             frame->payload_blob.size())) {
      return Status::kBoundsError;
    }
    RecordPayloadView view{};
    view.record_id = rec.record_id;
    view.old_offset = rec.payload_offset;
    view.payload_len = rec.payload_len;
    views.push_back(view);
  }

  std::sort(views.begin(), views.end(),
            [](const RecordPayloadView& a, const RecordPayloadView& b) {
              if (a.old_offset != b.old_offset) {
                return a.old_offset < b.old_offset;
              }
              return a.record_id < b.record_id;
            });

  std::vector<std::uint8_t> compact;
  compact.reserve(frame->payload_blob.size());
  std::size_t cursor = 0;

  for (RecordPayloadView& view : views) {
    view.new_offset = static_cast<std::uint32_t>(cursor);
    const std::uint8_t* src =
        frame->payload_blob.data() + view.old_offset;
    compact.insert(compact.end(), src, src + view.payload_len);
    cursor += view.payload_len;
  }

  for (WireBatchRecord& rec : frame->records) {
    if (rec.payload_len == 0) {
      rec.payload_offset = static_cast<std::uint32_t>(cursor);
      continue;
    }
    for (const RecordPayloadView& view : views) {
      if (view.record_id == rec.record_id) {
        rec.payload_offset = view.new_offset;
        break;
      }
    }
  }

  frame->payload_blob.swap(compact);
  return Status::kOk;
}

BatchNormalizeResult BatchNormalizer::NormalizeBatchRecords(
    BatchWireFrame* frame) {
  BatchNormalizeResult result{};
  result.status = Status::kOk;
  if (frame == nullptr) {
    result.status = Status::kBoundsError;
    return result;
  }

  result.frame = *frame;
  const std::size_t before = result.frame.payload_blob.size();

  result.status = ApplyDeskHints(&result.frame);
  if (result.status != Status::kOk) {
    return result;
  }

  if (config_.rewrite_record_offsets) {
    for (WireBatchRecord& rec : result.frame.records) {
      if (rec.payload_len > 0) {
        ++result.records_rewritten;
      }
    }
  }

  if (config_.apply_pro_rata_hints &&
      (result.frame.header.flags & kBatchFlagProRata) != 0) {
    for (WireBatchRecord& rec : result.frame.records) {
      if (rec.qty_milli > 0 && rec.price_tick == 0) {
        rec.price_tick = 100;
      }
    }
  }

  result.status = RewritePayloadLayout(&result.frame);
  if (result.status != Status::kOk) {
    return result;
  }

  if (result.frame.payload_blob.size() < before) {
    result.bytes_compacted =
        static_cast<std::uint32_t>(before - result.frame.payload_blob.size());
  }

  *frame = result.frame;
  return result;
}

}  // namespace engine
}  // namespace tkr
