#include "tkr/ingress/ingress_dispatch.h"

#include "tkr/util/bounds.h"

#include <cstring>

namespace tkr {
namespace ingress {
namespace {

std::uint32_t ReadU32Le(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint16_t ReadU16Le(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8);
}

struct EnvelopeWireLayout {
  WireEnvelopeHeader header;
  std::vector<WireEnvelopeChannel> channels;
  std::size_t total_bytes;
};

Status DecodeEnvelopeWire(const std::uint8_t* data, std::size_t size,
                          EnvelopeWireLayout* out) {
  if (out == nullptr || data == nullptr || size < 24) {
    return Status::kTruncated;
  }
  out->header.magic = ReadU32Le(data + 0);
  out->header.version = ReadU16Le(data + 4);
  out->header.channel_count = ReadU16Le(data + 6);
  out->header.flags = ReadU32Le(data + 8);
  out->header.ingress_seq = ReadU32Le(data + 12);
  out->header.parent_batch_id = ReadU32Le(data + 16);
  out->header.seal_digest = ReadU32Le(data + 20);

  if (out->header.magic != kEnvelopeMagic) {
    return Status::kInvalidMagic;
  }
  if (out->header.channel_count > kMaxEnvelopeChannels) {
    return Status::kBoundsError;
  }

  const std::size_t table_bytes =
      static_cast<std::size_t>(out->header.channel_count) * 20;
  if (!util::SectionBodyInBounds(24, table_bytes, size)) {
    return Status::kBoundsError;
  }

  out->channels.clear();
  out->channels.reserve(out->header.channel_count);
  std::size_t cursor = 24;
  for (std::uint16_t i = 0; i < out->header.channel_count; ++i) {
    WireEnvelopeChannel ch{};
    ch.channel_id = ReadU32Le(data + cursor + 0);
    ch.kind = ReadU32Le(data + cursor + 4);
    ch.payload_offset = ReadU32Le(data + cursor + 8);
    ch.payload_len = ReadU32Le(data + cursor + 12);
    ch.route_hint = ReadU32Le(data + cursor + 16);
    out->channels.push_back(ch);
    cursor += 20;
  }

  out->total_bytes = size;
  return Status::kOk;
}

}  // namespace

IngressDispatch::IngressDispatch(IngressDispatchConfig config)
    : config_(config),
      classifier_(IngressClassifierConfig{true, true, 10}),
      batch_codec_(wire::BatchCodecConfig{true, true, kMaxBatchRecords}),
      ingress_seq_(0) {}

void IngressDispatch::Reset() {
  pending_envelopes_.clear();
  temp_payload_heap_.clear();
  stats_ = IngressStreamStats{};
  ingress_seq_ = 0;
  fix_parser_.ResetSession();
}

Status IngressDispatch::AttachChannelViews(IngressEnvelope* envelope) {
  if (envelope == nullptr) {
    return Status::kBoundsError;
  }
  envelope->channel_views.clear();
  for (const WireEnvelopeChannel& ch : envelope->wire.channels) {
    if (ch.payload_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(ch.payload_offset, ch.payload_len,
                             envelope->owned_payload.size())) {
      return Status::kBoundsError;
    }
    ChannelView view{};
    view.channel_id = ch.channel_id;
    view.kind = ch.kind;
    view.payload_ptr = envelope->owned_payload.data() + ch.payload_offset;
    view.payload_len = ch.payload_len;
    view.route_hint = ch.route_hint;
    view.queued = false;
    envelope->channel_views.push_back(view);
  }
  return Status::kOk;
}

Status IngressDispatch::QueueEnvelopeChannelViews(IngressEnvelope* envelope) {
  if (envelope == nullptr) {
    return Status::kBoundsError;
  }
  if (!config_.queue_envelope_channels) {
    return Status::kOk;
  }
  for (ChannelView& view : envelope->channel_views) {
    view.queued = true;
  }
  return Status::kOk;
}

Status IngressDispatch::BuildEnvelopeFromWire(const wire::BatchDecodeResult& batch_result,
                                              IngressEnvelope* envelope) {
  if (envelope == nullptr) {
    return Status::kBoundsError;
  }
  envelope->wire.header.magic = kEnvelopeMagic;
  envelope->wire.header.version = kWireVersion;
  envelope->wire.header.channel_count = 1;
  envelope->wire.header.flags = kEnvelopeFlagNestedBatch;
  envelope->wire.header.ingress_seq = ++ingress_seq_;
  envelope->wire.header.parent_batch_id = batch_result.frame.header.desk_id;
  envelope->wire.header.seal_digest = 0;

  WireEnvelopeChannel ch{};
  ch.channel_id = 1;
  ch.kind = static_cast<std::uint32_t>(kIngressBatchWire);
  ch.payload_offset = 0;
  ch.payload_len = static_cast<std::uint32_t>(batch_result.frame.payload_blob.size());
  ch.route_hint = batch_result.frame.header.flags;
  envelope->wire.channels = {ch};
  envelope->owned_payload = batch_result.frame.payload_blob;
  envelope->source_kind = kIngressBatchWire;
  envelope->ingress_seq = envelope->wire.header.ingress_seq;

  return AttachChannelViews(envelope);
}

Status IngressDispatch::DispatchFixSegment(std::string_view segment,
                                           IngressStreamResult* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  if (!config_.parse_fix_sessions) {
    return Status::kOk;
  }
  wire::FixParseResult parsed = fix_parser_.ParseWithSession(segment);
  if (parsed.status != Status::kOk) {
    ++out->stats.rejected_frames;
    return parsed.status;
  }
  ++out->stats.fix_messages;
  ++out->stats.frames_seen;

  if (parsed.message.msg_type == wire::FixMsgType::kAllocationInstruction ||
      parsed.message.msg_type == wire::FixMsgType::kAllocationReport) {
    IngressEnvelope envelope;
    envelope.source_kind = kIngressFix44;
    envelope.ingress_seq = ++ingress_seq_;
    envelope.wire.header.magic = kEnvelopeMagic;
    envelope.wire.header.version = kWireVersion;
    envelope.wire.header.flags = kEnvelopeFlagIngressSweep;
    envelope.wire.header.ingress_seq = envelope.ingress_seq;
    envelope.owned_payload.assign(segment.begin(), segment.end());

    WireEnvelopeChannel ch{};
    ch.channel_id = envelope.ingress_seq;
    ch.kind = static_cast<std::uint32_t>(kIngressFix44);
    ch.payload_offset = 0;
    ch.payload_len = static_cast<std::uint32_t>(segment.size());
    envelope.wire.channels.push_back(ch);

    Status attach = AttachChannelViews(&envelope);
    if (attach != Status::kOk) {
      return attach;
    }
    QueueEnvelopeChannelViews(&envelope);
    pending_envelopes_.push_back(std::move(envelope));
    ++out->stats.envelopes_queued;
  }
  return Status::kOk;
}

Status IngressDispatch::DispatchSwiftSegment(std::string_view segment,
                                             IngressStreamResult* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  if (!config_.parse_swift_statements) {
    return Status::kOk;
  }
  wire::Mt940ScanResult scanned = swift_scanner_.Scan(segment);
  if (scanned.status != Status::kOk) {
    ++out->stats.rejected_frames;
    return scanned.status;
  }
  ++out->stats.swift_statements;
  ++out->stats.frames_seen;

  IngressEnvelope envelope;
  envelope.source_kind = kIngressSwiftMt940;
  envelope.ingress_seq = ++ingress_seq_;
  envelope.wire.header.magic = kEnvelopeMagic;
  envelope.wire.header.version = kWireVersion;
  envelope.wire.header.flags = kEnvelopeFlagChannelTape;
  envelope.wire.header.ingress_seq = envelope.ingress_seq;
  envelope.owned_payload.assign(segment.begin(), segment.end());

  WireEnvelopeChannel ch{};
  ch.channel_id = envelope.ingress_seq;
  ch.kind = static_cast<std::uint32_t>(kIngressSwiftMt940);
  ch.payload_offset = 0;
  ch.payload_len = static_cast<std::uint32_t>(segment.size());
  ch.route_hint = scanned.line_count;
  envelope.wire.channels.push_back(ch);

  Status attach = AttachChannelViews(&envelope);
  if (attach != Status::kOk) {
    return attach;
  }
  QueueEnvelopeChannelViews(&envelope);
  pending_envelopes_.push_back(std::move(envelope));
  ++out->stats.envelopes_queued;
  return Status::kOk;
}

Status IngressDispatch::DispatchBatchSegment(const std::uint8_t* data, std::size_t size,
                                             IngressStreamResult* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  wire::BatchDecodeResult decoded = batch_codec_.DecodeBatch(data, size);
  if (decoded.status != Status::kOk) {
    ++out->stats.rejected_frames;
    return decoded.status;
  }
  ++out->stats.batches_decoded;
  ++out->stats.frames_seen;
  out->decoded_batches.push_back(decoded.frame);

  IngressEnvelope envelope;
  Status built = BuildEnvelopeFromWire(decoded, &envelope);
  if (built != Status::kOk) {
    return built;
  }
  QueueEnvelopeChannelViews(&envelope);
  pending_envelopes_.push_back(std::move(envelope));
  ++out->stats.envelopes_queued;
  return Status::kOk;
}

Status IngressDispatch::DispatchEnvelopeSegment(const std::uint8_t* data, std::size_t size,
                                                IngressStreamResult* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  EnvelopeWireLayout layout;
  Status decoded = DecodeEnvelopeWire(data, size, &layout);
  if (decoded != Status::kOk) {
    ++out->stats.rejected_frames;
    return decoded;
  }

  std::size_t payload_offset = 24 + layout.channels.size() * 20;
  std::size_t payload_span = 0;
  for (const WireEnvelopeChannel& ch : layout.channels) {
    if (ch.payload_len == 0) {
      continue;
    }
    const std::size_t end =
        static_cast<std::size_t>(ch.payload_offset + ch.payload_len);
    if (end > payload_span) {
      payload_span = end;
    }
  }
  if (!util::SectionBodyInBounds(payload_offset, payload_span, size)) {
    return Status::kBoundsError;
  }

  auto owned = std::make_unique<std::vector<std::uint8_t>>();
  if (payload_span > 0) {
    owned->assign(data + payload_offset, data + payload_offset + payload_span);
  }

  IngressEnvelope envelope;
  envelope.wire.header = layout.header;
  envelope.wire.channels = layout.channels;
  envelope.source_kind = kIngressEnvelopeWire;
  envelope.ingress_seq = layout.header.ingress_seq;

  temp_payload_heap_.push_back(std::move(owned));
  std::vector<std::uint8_t>* heap_buf = temp_payload_heap_.back().get();

  const bool cross_frame_tape =
      (layout.header.flags & kEnvelopeFlagCrossFrameTape) != 0;
  if ((layout.header.flags & kEnvelopeFlagChannelTape) != 0 &&
      cross_frame_tape && heap_buf != nullptr) {
    envelope.owned_payload.clear();
    envelope.channel_views.clear();
    for (const WireEnvelopeChannel& ch : envelope.wire.channels) {
      if (ch.payload_len == 0) {
        continue;
      }
      if (!util::SliceInBounds(ch.payload_offset, ch.payload_len, heap_buf->size())) {
        return Status::kBoundsError;
      }
      ChannelView view{};
      view.channel_id = ch.channel_id;
      view.kind = ch.kind;
      view.payload_ptr = heap_buf->data() + ch.payload_offset;
      view.payload_len = ch.payload_len;
      view.route_hint = ch.route_hint;
      view.queued = false;
      envelope.channel_views.push_back(view);
    }
  } else {
    envelope.owned_payload = *heap_buf;
    Status attach = AttachChannelViews(&envelope);
    if (attach != Status::kOk) {
      temp_payload_heap_.pop_back();
      return attach;
    }
  }
  QueueEnvelopeChannelViews(&envelope);
  pending_envelopes_.push_back(std::move(envelope));
  ++out->stats.frames_seen;
  ++out->stats.envelopes_queued;
  return Status::kOk;
}

Status IngressDispatch::DispatchSessionSegment(const std::uint8_t* data, std::size_t size,
                                               IngressStreamResult* out) {
  if (out == nullptr || size < 24) {
    return Status::kTruncated;
  }
  const std::uint32_t magic = ReadU32Le(data);
  if (magic != kSessionMagic) {
    return Status::kInvalidMagic;
  }
  ++out->stats.frames_seen;
  return Status::kOk;
}

Status IngressDispatch::CommitIngressSweep() {
  temp_payload_heap_.clear();
  pending_envelopes_.clear();
  return Status::kOk;
}

Status IngressDispatch::SealPendingEnvelopes() {
  while (!pending_envelopes_.empty()) {
    IngressEnvelope envelope = std::move(pending_envelopes_.front());
    pending_envelopes_.pop_front();

    std::uint32_t seal = 2166136261u;
    for (const ChannelView& view : envelope.channel_views) {
      if (view.payload_ptr != nullptr && view.payload_len > 0) {
        seal = util::Fnv1a32(view.payload_ptr, view.payload_len) ^ (seal * 16777619u);
      }
    }
    envelope.wire.header.seal_digest = seal;
  }
  return Status::kOk;
}

IngressStreamResult IngressDispatch::ProcessIngressStream(const std::uint8_t* data,
                                                          std::size_t size) {
  IngressStreamResult result{};
  result.status = Status::kOk;

  if (data == nullptr || size == 0) {
    result.status = Status::kTruncated;
    return result;
  }

  IngressClassifyResult classified = classifier_.ClassifyBinary(data, size);
  if (classified.status != Status::kOk &&
      classified.primary_kind == kIngressUnknown) {
    result.status = classified.status;
    return result;
  }

  std::string_view view(reinterpret_cast<const char*>(data), size);

  switch (classified.primary_kind) {
    case kIngressFix44:
      result.status = DispatchFixSegment(view, &result);
      break;
    case kIngressSwiftMt940:
      result.status = DispatchSwiftSegment(view, &result);
      break;
    case kIngressBatchWire:
      result.status = DispatchBatchSegment(data, size, &result);
      break;
    case kIngressEnvelopeWire:
      result.status = DispatchEnvelopeSegment(data, size, &result);
      break;
    case kIngressSessionWire:
      result.status = DispatchSessionSegment(data, size, &result);
      break;
    case kIngressMixedStream: {
      Status fix_st = DispatchFixSegment(view, &result);
      if (fix_st != Status::kOk) {
        Status swift_st = DispatchSwiftSegment(view, &result);
        if (swift_st != Status::kOk) {
          result.status = DispatchBatchSegment(data, size, &result);
        }
      }
      break;
    }
    default:
      result.status = Status::kUnknownFormat;
      ++result.stats.rejected_frames;
      return result;
  }

  if (result.status == Status::kOk && !pending_envelopes_.empty()) {
    for (IngressEnvelope& env : pending_envelopes_) {
      result.completed_envelopes.push_back(env);
    }
  }

  result.stats = stats_;
  return result;
}

IngressStreamResult IngressDispatch::ProcessIngressStream(std::string_view text) {
  return ProcessIngressStream(reinterpret_cast<const std::uint8_t*>(text.data()),
                              text.size());
}

}  // namespace ingress
}  // namespace tkr
