#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace wire {

struct SessionCodecConfig {
  bool enable_merge_staging;
  bool validate_ref_bounds;
  std::uint32_t max_legs;
};

struct SessionEncodeResult {
  Status status;
  std::vector<std::uint8_t> wire_bytes;
  std::uint32_t merge_digest;
  std::size_t ref_blob_size;
};

struct SessionDecodeResult {
  Status status;
  SessionWireFrame frame;
  std::size_t consumed_bytes;
};

class SessionWireCodec {
 public:
  explicit SessionWireCodec(SessionCodecConfig config);

  SessionEncodeResult EncodeSession(const WireSessionHeader& header,
                                    const std::vector<WireSessionLeg>& legs,
                                    const std::uint8_t* ref_data,
                                    std::size_t ref_size);

  SessionDecodeResult DecodeSession(const std::uint8_t* data, std::size_t size);
  SessionDecodeResult DecodePartial(const std::uint8_t* data, std::size_t size);

  SessionEncodeResult EncodeLegsOnly(
      const std::vector<WireSessionLeg>& legs,
      const std::vector<std::uint8_t>& ref_blob);

  Status StageMergeSlots(SessionWireFrame* frame);
  Status ClearMergeSlots(SessionWireFrame* frame);
  Status ValidateLegRefs(const SessionWireFrame& frame) const;
  Status PatchLegRefOffset(WireSessionLeg* leg, std::uint32_t new_offset);
  Status RelocateRefBlob(SessionWireFrame* frame);

  static std::uint32_t EstimateEncodedSize(std::uint16_t leg_count,
                                           std::size_t ref_size);

  std::uint32_t ComputeHeaderDigest(const WireSessionHeader& header) const;
  std::uint32_t ComputeRefDigest(const std::uint8_t* data,
                                 std::size_t len) const;
  std::uint32_t ComputeLegTableDigest(
      const std::vector<WireSessionLeg>& legs) const;

  const std::vector<MergeSlot>& MergeSlots() const { return merge_table_; }

 private:
  Status ValidateHeader(const WireSessionHeader& header) const;
  Status ValidateLegBounds(const WireSessionLeg& leg,
                           std::size_t ref_size) const;
  Status ReadHeader(const std::uint8_t* data, std::size_t size,
                    WireSessionHeader* out, std::size_t* consumed);
  Status ReadLegs(const std::uint8_t* data, std::size_t size,
                  std::size_t offset, std::uint16_t count,
                  std::vector<WireSessionLeg>* out);
  Status ReadRefBlob(const std::uint8_t* data, std::size_t size,
                     std::size_t offset, std::vector<std::uint8_t>* out);

  void AppendU32Le(std::vector<std::uint8_t>* out, std::uint32_t value);
  void AppendU16Le(std::vector<std::uint8_t>* out, std::uint16_t value);
  void AppendLegBytes(std::vector<std::uint8_t>* out,
                      const WireSessionLeg& leg);

  SessionCodecConfig config_;
  std::vector<MergeSlot> merge_table_;
  std::uint32_t next_slot_id_;
};

}  // namespace wire
}  // namespace tkr
