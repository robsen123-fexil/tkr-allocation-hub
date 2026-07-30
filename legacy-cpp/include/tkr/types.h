#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tkr {

// Wire magics — ASCII "TKR1", "TKR2", "TKR3"
constexpr std::uint32_t kBatchMagic = 0x544B5231u;
constexpr std::uint32_t kEnvelopeMagic = 0x544B5232u;
constexpr std::uint32_t kSessionMagic = 0x544B5233u;

constexpr std::uint16_t kWireVersion = 7;
constexpr std::size_t kMaxBatchRecords = 4096;
constexpr std::size_t kMaxEnvelopeChannels = 256;
constexpr std::size_t kMaxSessionLegs = 512;
constexpr std::size_t kMaxDeferredSlots = 1024;

enum class Status : std::uint8_t {
  kOk = 0,
  kInvalidMagic,
  kTruncated,
  kBoundsError,
  kDuplicateKey,
  kComplianceReject,
  kMarginBreach,
  kUnknownFormat,
  kSessionGap,
  kPartialFrame,
};

enum BatchFlags : std::uint32_t {
  kBatchFlagNone = 0,
  kBatchFlagProRata = 1u << 0,
  kBatchFlagMarginCheck = 1u << 1,
  kBatchFlagComplianceHold = 1u << 2,
  kBatchFlagDeferredDigest = 1u << 3,
  kBatchFlagPartialFill = 1u << 4,
  kBatchFlagCrossDesk = 1u << 5,
  kBatchFlagCrossFrameDefer = 1u << 6,
};

enum EnvelopeFlags : std::uint32_t {
  kEnvelopeFlagNone = 0,
  kEnvelopeFlagNestedBatch = 1u << 0,
  kEnvelopeFlagChannelTape = 1u << 1,
  kEnvelopeFlagSealPending = 1u << 2,
  kEnvelopeFlagIngressSweep = 1u << 3,
  kEnvelopeFlagMarginEnvelope = 1u << 4,
};

enum SessionLegFlags : std::uint32_t {
  kLegFlagNone = 0,
  kLegFlagAllocationSlice = 1u << 0,
  kLegFlagMergePending = 1u << 1,
  kLegFlagComplianceCleared = 1u << 2,
  kLegFlagMarginReserved = 1u << 3,
  kLegFlagCheckpointPin = 1u << 4,
};

enum IngressKind : std::uint8_t {
  kIngressUnknown = 0,
  kIngressFix44,
  kIngressSwiftMt940,
  kIngressBatchWire,
  kIngressEnvelopeWire,
  kIngressSessionWire,
  kIngressMixedStream,
};

#pragma pack(push, 1)

struct WireBatchHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t header_bytes;
  std::uint32_t record_count;
  std::uint32_t flags;
  std::uint32_t desk_id;
  std::uint32_t trade_date_yyyymmdd;
  std::uint32_t payload_digest;
};

struct WireBatchRecord {
  std::uint32_t record_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::uint32_t qty_milli;
  std::uint32_t price_tick;
  std::uint32_t payload_offset;
  std::uint32_t payload_len;
  std::uint32_t flags;
};

struct WireEnvelopeHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t channel_count;
  std::uint32_t flags;
  std::uint32_t ingress_seq;
  std::uint32_t parent_batch_id;
  std::uint32_t seal_digest;
};

struct WireEnvelopeChannel {
  std::uint32_t channel_id;
  std::uint32_t kind;
  std::uint32_t payload_offset;
  std::uint32_t payload_len;
  std::uint32_t route_hint;
};

struct WireEnvelope {
  WireEnvelopeHeader header;
  std::vector<WireEnvelopeChannel> channels;
};

struct WireSessionHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t leg_count;
  std::uint32_t session_id;
  std::uint32_t checkpoint_seq;
  std::uint32_t flags;
  std::uint32_t merge_digest;
};

struct WireSessionLeg {
  std::uint32_t leg_id;
  std::uint32_t cl_ord_id;
  std::uint32_t alloc_account;
  std::uint32_t qty_milli;
  std::uint32_t ref_offset;
  std::uint32_t ref_len;
  std::uint32_t flags;
};

#pragma pack(pop)

struct DeferredSlot {
  std::uint32_t slot_id;
  std::uint32_t record_id;
  const std::uint8_t* payload_ptr;
  std::uint32_t payload_len;
  std::uint32_t staging_flags;
  bool active;
};

struct ChannelView {
  std::uint32_t channel_id;
  std::uint32_t kind;
  const std::uint8_t* payload_ptr;
  std::uint32_t payload_len;
  std::uint32_t route_hint;
  bool queued;
};

struct MergeSlot {
  std::uint32_t slot_id;
  std::uint32_t leg_id;
  const std::uint8_t* ref_ptr;
  std::uint32_t ref_len;
  std::uint32_t checkpoint_seq;
  bool pinned;
};

struct BatchWireFrame {
  WireBatchHeader header;
  std::vector<WireBatchRecord> records;
  std::vector<std::uint8_t> payload_blob;
  std::vector<DeferredSlot> deferred_slots;
};

struct IngressEnvelope {
  WireEnvelope wire;
  std::vector<std::uint8_t> owned_payload;
  std::vector<ChannelView> channel_views;
  IngressKind source_kind;
  std::uint32_t ingress_seq;
};

struct SessionWireFrame {
  WireSessionHeader header;
  std::vector<WireSessionLeg> legs;
  std::vector<std::uint8_t> ref_blob;
  std::vector<MergeSlot> merge_slots;
};

struct AllocationBatchSummary {
  std::uint32_t batch_id;
  std::uint32_t record_count;
  std::uint32_t total_qty_milli;
  std::uint32_t desk_id;
  std::uint32_t flags;
  bool margin_cleared;
  bool compliance_cleared;
};

struct MarginCheckResult {
  std::uint32_t account_id;
  std::int64_t required_margin_cents;
  std::int64_t available_margin_cents;
  bool passed;
};

inline const char* StatusToString(Status status) {
  switch (status) {
    case Status::kOk:
      return "ok";
    case Status::kInvalidMagic:
      return "invalid_magic";
    case Status::kTruncated:
      return "truncated";
    case Status::kBoundsError:
      return "bounds_error";
    case Status::kDuplicateKey:
      return "duplicate_key";
    case Status::kComplianceReject:
      return "compliance_reject";
    case Status::kMarginBreach:
      return "margin_breach";
    case Status::kUnknownFormat:
      return "unknown_format";
    case Status::kSessionGap:
      return "session_gap";
    case Status::kPartialFrame:
      return "partial_frame";
    default:
      return "unknown";
  }
}

}  // namespace tkr
