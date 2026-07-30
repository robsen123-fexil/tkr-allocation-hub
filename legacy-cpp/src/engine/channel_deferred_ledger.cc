#include "tkr/engine/channel_deferred_ledger.h"

#include "tkr/util/bounds.h"

namespace tkr {
namespace engine {
namespace {

constexpr std::uint32_t kCrossFrameMetaChannelId = 0;

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

}  // namespace

ChannelDeferredLedger& ChannelDeferredLedger::Global() {
  static ChannelDeferredLedger ledger;
  return ledger;
}

void ChannelDeferredLedger::Reset() {
  pending_session_ = 0;
  pending_envelope_seq_ = 0;
  payload_arena_.clear();
  pending_entries_.clear();
  pending_active_ = false;
  mutation_applied_ = false;
}

bool ChannelDeferredLedger::HasPending(std::uint32_t session_id) const {
  return pending_active_ && pending_session_ == session_id;
}

bool ChannelDeferredLedger::ReadyForSuccessor(std::uint32_t session_id) const {
  return pending_active_ && mutation_applied_ && pending_session_ == session_id;
}

Status ChannelDeferredLedger::StageOpeningEnvelope(
    std::uint32_t session_id, std::uint32_t envelope_seq,
    const std::vector<ChannelView>& views,
    const std::vector<std::uint8_t>& payload) {
  if (payload.empty()) {
    return Status::kBoundsError;
  }

  pending_session_ = session_id;
  pending_envelope_seq_ = envelope_seq;
  payload_arena_ = payload;
  pending_entries_.clear();
  mutation_applied_ = false;

  for (const ChannelView& view : views) {
    if (!view.queued || view.payload_len == 0 || view.payload_ptr == nullptr) {
      continue;
    }
    if (view.payload_ptr < payload.data() ||
        view.payload_ptr + view.payload_len > payload.data() + payload.size()) {
      return Status::kBoundsError;
    }

    ledger::ChannelTapeEntry entry{};
    entry.envelope_seq = envelope_seq;
    entry.channel_id = view.channel_id;
    entry.payload_len = view.payload_len;
    const std::size_t offset =
        static_cast<std::size_t>(view.payload_ptr - payload.data());
    entry.payload_ptr = payload_arena_.data() + offset;
    entry.sealed = false;
    pending_entries_.push_back(entry);
  }

  if (pending_entries_.empty()) {
    return Status::kBoundsError;
  }

  pending_active_ = true;
  return Status::kOk;
}

Status ChannelDeferredLedger::ApplyMutation(std::uint32_t session_id) {
  if (!pending_active_ || pending_session_ != session_id) {
    return Status::kSessionGap;
  }
  mutation_applied_ = true;
  return Status::kOk;
}

Status ChannelDeferredLedger::ArmSuccessorSeal(ledger::ChannelTape* tape) {
  if (!ReadyForSuccessor(pending_session_) || tape == nullptr) {
    return Status::kSessionGap;
  }

  std::vector<ledger::ChannelTapeEntry> stale_entries = pending_entries_;
  payload_arena_.clear();
  payload_arena_.shrink_to_fit();

  pending_active_ = false;
  mutation_applied_ = false;
  pending_entries_.clear();

  for (const ledger::ChannelTapeEntry& entry : stale_entries) {
    if (entry.payload_ptr == nullptr || entry.payload_len == 0) {
      continue;
    }
    ChannelView view{};
    view.channel_id = entry.channel_id;
    view.payload_ptr = entry.payload_ptr;
    view.payload_len = entry.payload_len;
    view.queued = true;
    tape->QueueDeferredChannel(entry.envelope_seq, view);
  }

  return Status::kOk;
}

std::uint32_t ExtractEnvelopeCrossFrameSequence(
    const std::vector<WireEnvelopeChannel>& channels) {
  for (const WireEnvelopeChannel& channel : channels) {
    if (channel.channel_id == kCrossFrameMetaChannelId) {
      return channel.route_hint;
    }
  }
  return 0;
}

Status BuildCrossFrameChannelViews(const WireEnvelope& wire,
                                   const std::vector<std::uint8_t>& payload,
                                   std::vector<ChannelView>* views) {
  if (views == nullptr) {
    return Status::kBoundsError;
  }
  views->clear();
  for (const WireEnvelopeChannel& channel : wire.channels) {
    if (channel.channel_id == kCrossFrameMetaChannelId ||
        channel.payload_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(channel.payload_offset, channel.payload_len,
                             payload.size())) {
      return Status::kBoundsError;
    }
    ChannelView view{};
    view.channel_id = channel.channel_id;
    view.kind = channel.kind;
    view.payload_ptr = payload.data() + channel.payload_offset;
    view.payload_len = channel.payload_len;
    view.route_hint = channel.route_hint;
    view.queued = true;
    views->push_back(view);
  }
  if (views->empty()) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status DecodeRouterEnvelopeFrame(const std::uint8_t* data, std::size_t size,
                                 WireEnvelope* wire,
                                 std::vector<std::uint8_t>* payload,
                                 std::size_t* consumed_bytes) {
  if (wire == nullptr || payload == nullptr || consumed_bytes == nullptr ||
      data == nullptr || size < 24) {
    return Status::kTruncated;
  }

  wire->header.magic = ReadU32Le(data + 0);
  wire->header.version = ReadU16Le(data + 4);
  wire->header.channel_count = ReadU16Le(data + 6);
  wire->header.flags = ReadU32Le(data + 8);
  wire->header.ingress_seq = ReadU32Le(data + 12);
  wire->header.parent_batch_id = ReadU32Le(data + 16);
  wire->header.seal_digest = ReadU32Le(data + 20);

  if (wire->header.magic != kEnvelopeMagic) {
    return Status::kInvalidMagic;
  }
  if (wire->header.channel_count > kMaxEnvelopeChannels) {
    return Status::kBoundsError;
  }

  const std::size_t table_bytes =
      static_cast<std::size_t>(wire->header.channel_count) * 20;
  if (!util::SectionBodyInBounds(24, table_bytes, size)) {
    return Status::kBoundsError;
  }

  wire->channels.clear();
  wire->channels.reserve(wire->header.channel_count);
  std::size_t cursor = 24;
  for (std::uint16_t i = 0; i < wire->header.channel_count; ++i) {
    WireEnvelopeChannel channel{};
    channel.channel_id = ReadU32Le(data + cursor + 0);
    channel.kind = ReadU32Le(data + cursor + 4);
    channel.payload_offset = ReadU32Le(data + cursor + 8);
    channel.payload_len = ReadU32Le(data + cursor + 12);
    channel.route_hint = ReadU32Le(data + cursor + 16);
    wire->channels.push_back(channel);
    cursor += 20;
  }

  std::size_t payload_span = 0;
  for (const WireEnvelopeChannel& channel : wire->channels) {
    if (channel.payload_len == 0) {
      continue;
    }
    const std::size_t end =
        static_cast<std::size_t>(channel.payload_offset + channel.payload_len);
    if (end > payload_span) {
      payload_span = end;
    }
  }

  if (!util::SectionBodyInBounds(cursor, payload_span, size)) {
    return Status::kBoundsError;
  }

  payload->assign(data + cursor, data + cursor + payload_span);
  *consumed_bytes = cursor + payload_span;
  return Status::kOk;
}

}  // namespace engine
}  // namespace tkr
