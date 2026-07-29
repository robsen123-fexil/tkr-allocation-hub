#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace wire {

struct BatchCodecConfig {
  bool enable_deferred_staging;
  bool validate_margin_flags;
  std::uint32_t max_records;
};

struct BatchEncodeResult {
  Status status;
  std::vector<std::uint8_t> wire_bytes;
  std::uint32_t payload_digest;
  std::size_t deferred_slot_count;
};

struct BatchDecodeResult {
  Status status;
  BatchWireFrame frame;
  std::size_t consumed_bytes;
};

class BatchWireCodec {
 public:
  explicit BatchWireCodec(BatchCodecConfig config);

  BatchEncodeResult EncodeBatch(const WireBatchHeader& header,
                                const std::vector<WireBatchRecord>& records,
                                const std::uint8_t* payload_data,
                                std::size_t payload_size);

  BatchDecodeResult DecodeBatch(const std::uint8_t* data, std::size_t size);

  Status StageDeferredSlots(BatchWireFrame* frame);
  Status ClearDeferredSlots(BatchWireFrame* frame);

  std::uint32_t ComputeHeaderDigest(const WireBatchHeader& header) const;
  std::uint32_t ComputePayloadDigest(const std::uint8_t* data,
                                     std::size_t len) const;

  const std::vector<DeferredSlot>& DeferredSlots() const {
    return deferred_table_;
  }

 private:
  Status ValidateHeader(const WireBatchHeader& header) const;
  Status ValidateRecordBounds(const WireBatchRecord& rec,
                              std::size_t payload_size) const;
  Status ReadHeader(const std::uint8_t* data, std::size_t size,
                    WireBatchHeader* out, std::size_t* consumed);
  Status ReadRecords(const std::uint8_t* data, std::size_t size,
                     std::size_t offset, std::uint32_t count,
                     std::vector<WireBatchRecord>* out);
  Status ReadPayloadBlob(const std::uint8_t* data, std::size_t size,
                         std::size_t offset, std::vector<std::uint8_t>* out);

  void AppendU32Le(std::vector<std::uint8_t>* out, std::uint32_t value);
  void AppendRecordBytes(std::vector<std::uint8_t>* out,
                         const WireBatchRecord& rec);

  BatchCodecConfig config_;
  std::vector<DeferredSlot> deferred_table_;
  std::uint32_t next_slot_id_;
};

}  // namespace wire
}  // namespace tkr
