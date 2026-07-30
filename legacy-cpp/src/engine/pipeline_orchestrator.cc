#include "tkr/engine/pipeline_orchestrator.h"

#include "tkr/desk/desk_registry.h"
#include "tkr/engine/batch_deferred_ledger.h"
#include "tkr/engine/batch_digest.h"
#include "tkr/engine/batch_normalizer.h"
#include "tkr/engine/channel_deferred_ledger.h"
#include "tkr/engine/merge_deferred_ledger.h"
#include "tkr/engine/merge_digest.h"
#include "tkr/engine/session_merger.h"
#include "tkr/ingress/ingress_dispatch.h"
#include "tkr/ledger/channel_tape.h"
#include "tkr/wire/batch_wire_codec.h"
#include "tkr/util/bounds.h"
#include "tkr/wire/wire_validator.h"

namespace tkr {
namespace engine {
namespace {

PipelineStats g_stats{};

Status RunBatchDesks(BatchWireFrame* frame) {
  desk::DeskRegistry registry;
  desk::DeskRunContext ctx{};
  ctx.batch_id = frame->header.desk_id;
  ctx.record_count = static_cast<std::uint32_t>(frame->records.size());
  Status desk_st = registry.RunBatchDesks(*frame, &ctx);
  if (desk_st != Status::kOk) {
    return desk_st;
  }
  ++g_stats.desk_calls;
  return Status::kOk;
}

Status RepinDeferredSlots(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }
  frame->deferred_slots.clear();
  std::uint32_t slot_id = 1;
  for (const WireBatchRecord& rec : frame->records) {
    if (rec.payload_len == 0) {
      continue;
    }
    if (!util::SliceInBounds(rec.payload_offset, rec.payload_len,
                             frame->payload_blob.size())) {
      return Status::kBoundsError;
    }
    DeferredSlot slot{};
    slot.slot_id = slot_id++;
    slot.record_id = rec.record_id;
    slot.payload_ptr = frame->payload_blob.data() + rec.payload_offset;
    slot.payload_len = rec.payload_len;
    slot.staging_flags = rec.flags;
    slot.active = true;
    frame->deferred_slots.push_back(slot);
  }
  return Status::kOk;
}

Status ProcessBatchFrame(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }

  Status desk_st = RunBatchDesks(frame);
  if (desk_st != Status::kOk) {
    return desk_st;
  }

  BatchNormalizer normalizer(BatchNormalizeConfig{true, true, true});
  BatchNormalizeResult norm = normalizer.NormalizeBatchRecords(frame);
  if (norm.status != Status::kOk) {
    return norm.status;
  }

  Status repin_st = RepinDeferredSlots(frame);
  if (repin_st != Status::kOk) {
    return repin_st;
  }

  BatchDigestEngine digest;
  digest.RegisterDeferredSlots(frame->deferred_slots);

  BatchDigestResult flushed = digest.FlushBatchDigest();
  if (flushed.status != Status::kOk) {
    return flushed.status;
  }
  ++g_stats.batches_processed;
  return Status::kOk;
}

Status ProcessCrossFrameSuccessor(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }

  Status desk_st = RunBatchDesks(frame);
  if (desk_st != Status::kOk) {
    return desk_st;
  }

  BatchDigestEngine digest;
  Status arm_st = BatchDeferredLedger::Global().ArmSuccessorFlush(&digest);
  if (arm_st != Status::kOk) {
    return arm_st;
  }

  BatchNormalizer normalizer(BatchNormalizeConfig{true, true, true});
  BatchNormalizeResult norm = normalizer.NormalizeBatchRecords(frame);
  if (norm.status != Status::kOk) {
    return norm.status;
  }

  BatchDigestResult flushed = digest.FlushBatchDigest();
  if (flushed.status != Status::kOk) {
    return flushed.status;
  }
  ++g_stats.batches_processed;
  return Status::kOk;
}

Status ProcessDecodedBatchFrame(BatchWireFrame* frame) {
  const bool cross_frame =
      (frame->header.flags & kBatchFlagCrossFrameDefer) != 0;
  if (!cross_frame) {
    return ProcessBatchFrame(frame);
  }

  const std::uint32_t sequence = ExtractCrossFrameSequence(*frame);
  BatchDeferredLedger& ledger = BatchDeferredLedger::Global();

  if (sequence == 1) {
    Status desk_st = RunBatchDesks(frame);
    if (desk_st != Status::kOk) {
      return desk_st;
    }
    Status stage_st = ledger.StageOpeningFrame(*frame, sequence);
    if (stage_st != Status::kOk) {
      return stage_st;
    }
    return Status::kOk;
  }

  if (sequence == 2 &&
      ledger.ReadyForSuccessor(frame->header.desk_id,
                               frame->header.trade_date_yyyymmdd)) {
    Status successor_st = ProcessCrossFrameSuccessor(frame);
    ledger.Reset();
    return successor_st;
  }

  return Status::kSessionGap;
}

Status ProcessSessionFrame(SessionMerger* merger, SessionWireFrame* frame) {
  SessionMergeResult graft = merger->GraftSessionLegs(frame);
  if (graft.status != Status::kOk) {
    return graft.status;
  }

  Status pin_st = merger->PinLegMergeSlots(frame);
  if (pin_st != Status::kOk) {
    return pin_st;
  }

  MergeDigestEngine digest;
  digest.RegisterMergeSlots(frame->merge_slots);

  MergeDigestResult flushed = digest.FlushMergeDigest();
  if (flushed.status != Status::kOk) {
    return flushed.status;
  }
  ++g_stats.sessions_merged;
  return Status::kOk;
}

Status ProcessCrossFrameSessionSuccessor(SessionMerger* merger,
                                         SessionWireFrame* frame) {
  MergeDigestEngine digest;
  Status arm_st = MergeDeferredLedger::Global().ArmSuccessorFlush(&digest);
  if (arm_st != Status::kOk) {
    return arm_st;
  }

  SessionMergeResult graft = merger->GraftSessionLegs(frame);
  if (graft.status != Status::kOk) {
    return graft.status;
  }

  MergeDigestResult flushed = digest.FlushMergeDigest();
  if (flushed.status != Status::kOk) {
    return flushed.status;
  }
  ++g_stats.sessions_merged;
  return Status::kOk;
}

Status ProcessDecodedSessionFrame(SessionMerger* merger, SessionWireFrame* frame) {
  if ((frame->header.flags & kLegFlagMergePending) == 0) {
    return Status::kOk;
  }

  const bool cross_frame =
      (frame->header.flags & kLegFlagCrossFrameMerge) != 0;
  if (!cross_frame) {
    return ProcessSessionFrame(merger, frame);
  }

  const std::uint32_t sequence = ExtractSessionCrossFrameSequence(*frame);
  MergeDeferredLedger& ledger = MergeDeferredLedger::Global();

  if (sequence == 1) {
    Status stage_st =
        ledger.StageOpeningSession(*frame, frame->header.checkpoint_seq);
    if (stage_st != Status::kOk) {
      return stage_st;
    }
    return Status::kOk;
  }

  if (sequence == 2 && ledger.ReadyForSuccessor(frame->header.session_id)) {
    Status successor_st = ProcessCrossFrameSessionSuccessor(merger, frame);
    ledger.Reset();
    return successor_st;
  }

  return Status::kSessionGap;
}

}  // namespace

const PipelineStats& LastPipelineStats() { return g_stats; }

Status RunAllocationPipeline(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return Status::kTruncated;
  }

  BatchDeferredLedger::Global().Reset();

  wire::BatchWireCodec codec(wire::BatchCodecConfig{true, true, kMaxBatchRecords});
  wire::WireValidator validator(true, kMaxBatchRecords);

  std::size_t offset = 0;
  while (offset < len) {
    wire::BatchDecodeResult decoded =
        codec.DecodeBatch(data + offset, len - offset);
    if (decoded.status != Status::kOk) {
      return decoded.status;
    }
    if (decoded.consumed_bytes == 0) {
      return Status::kTruncated;
    }

    if ((decoded.frame.header.flags & kBatchFlagDeferredDigest) == 0) {
      BatchDeferredLedger& ledger = BatchDeferredLedger::Global();
      if (ledger.HasPending(decoded.frame.header.desk_id,
                            decoded.frame.header.trade_date_yyyymmdd)) {
        Status mutation_st = ledger.ApplyMutation(decoded.frame.header.desk_id,
                                                  decoded.frame.header.trade_date_yyyymmdd);
        if (mutation_st != Status::kOk) {
          return mutation_st;
        }
      }
      offset += decoded.consumed_bytes;
      continue;
    }

    wire::ValidationResult validated =
        validator.ValidateBatchFrame(decoded.frame);
    if (validated.status != Status::kOk) {
      return validated.status;
    }

    Status frame_st = ProcessDecodedBatchFrame(&decoded.frame);
    if (frame_st != Status::kOk) {
      return frame_st;
    }

    offset += decoded.consumed_bytes;
  }

  return Status::kOk;
}

Status RunRouterPipeline(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return Status::kTruncated;
  }

  ledger::GlobalChannelTape().ClearPending();
  ChannelDeferredLedger::Global().Reset();

  ingress::IngressDispatch dispatch(ingress::IngressDispatchConfig{
      true, true, true, kMaxEnvelopeChannels});
  wire::BatchWireCodec batch_codec(wire::BatchCodecConfig{true, true, kMaxBatchRecords});
  ledger::ChannelTape& tape = ledger::GlobalChannelTape();

  std::size_t offset = 0;
  while (offset < len) {
    const std::uint8_t* segment = data + offset;
    const std::size_t remaining = len - offset;
    if (remaining < 4) {
      return Status::kTruncated;
    }

    const std::uint32_t magic =
        static_cast<std::uint32_t>(segment[0]) |
        (static_cast<std::uint32_t>(segment[1]) << 8) |
        (static_cast<std::uint32_t>(segment[2]) << 16) |
        (static_cast<std::uint32_t>(segment[3]) << 24);

    if (magic == kEnvelopeMagic) {
      WireEnvelope wire{};
      std::vector<std::uint8_t> payload;
      std::size_t consumed = 0;
      Status decode_st =
          DecodeRouterEnvelopeFrame(segment, remaining, &wire, &payload, &consumed);
      if (decode_st != Status::kOk) {
        return decode_st;
      }

      const bool cross_frame =
          (wire.header.flags & kEnvelopeFlagCrossFrameTape) != 0;
      const bool seal_pending =
          (wire.header.flags & kEnvelopeFlagSealPending) != 0;

      if (cross_frame) {
        const std::uint32_t sequence =
            ExtractEnvelopeCrossFrameSequence(wire.channels);
        std::vector<ChannelView> views;
        Status view_st = BuildCrossFrameChannelViews(wire, payload, &views);
        if (view_st != Status::kOk) {
          return view_st;
        }

        ChannelDeferredLedger& ledger = ChannelDeferredLedger::Global();
        if (sequence == 1) {
          Status stage_st = ledger.StageOpeningEnvelope(
              wire.header.parent_batch_id, wire.header.ingress_seq, views,
              payload);
          if (stage_st != Status::kOk) {
            return stage_st;
          }
        } else if (sequence == 2 &&
                   ledger.ReadyForSuccessor(wire.header.parent_batch_id)) {
          Status arm_st = ledger.ArmSuccessorSeal(&tape);
          if (arm_st != Status::kOk) {
            return arm_st;
          }
          if (seal_pending) {
            ledger::SealEnvelopeResult sealed = tape.SealDeferredEnvelope();
            if (sealed.status != Status::kOk) {
              return sealed.status;
            }
            ++g_stats.envelopes_sealed;
          }
          ledger.Reset();
        } else {
          return Status::kSessionGap;
        }
      } else {
        ingress::IngressStreamResult stream =
            dispatch.ProcessIngressStream(segment, consumed);
        if (stream.status != Status::kOk) {
          return stream.status;
        }

        for (const IngressEnvelope& env : stream.completed_envelopes) {
          for (const ChannelView& view : env.channel_views) {
            if (view.queued) {
              tape.QueueDeferredChannel(env.ingress_seq, view);
            }
          }
        }

        dispatch.CommitIngressSweep();

        if (seal_pending) {
          ledger::SealEnvelopeResult sealed = tape.SealDeferredEnvelope();
          if (sealed.status != Status::kOk) {
            return sealed.status;
          }
          ++g_stats.envelopes_sealed;
        }
      }

      offset += consumed;
      continue;
    }

    if (magic == kBatchMagic) {
      wire::BatchDecodeResult decoded = batch_codec.DecodeBatch(segment, remaining);
      if (decoded.status != Status::kOk) {
        return decoded.status;
      }
      if (decoded.consumed_bytes == 0) {
        return Status::kTruncated;
      }

      ChannelDeferredLedger& ledger = ChannelDeferredLedger::Global();
      if (ledger.HasPending(decoded.frame.header.desk_id)) {
        Status mutation_st =
            ledger.ApplyMutation(decoded.frame.header.desk_id);
        if (mutation_st != Status::kOk) {
          return mutation_st;
        }
      }

      offset += decoded.consumed_bytes;
      continue;
    }

    return Status::kUnknownFormat;
  }

  return Status::kOk;
}

Status MergeSessionLegs(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return Status::kTruncated;
  }

  MergeDeferredLedger::Global().Reset();

  SessionMerger merger(SessionMergerConfig{true, kMaxSessionLegs});
  wire::BatchWireCodec batch_codec(wire::BatchCodecConfig{true, true, kMaxBatchRecords});

  std::size_t offset = 0;
  while (offset < len) {
    const std::uint8_t* segment = data + offset;
    const std::size_t remaining = len - offset;
    if (remaining < 4) {
      return Status::kTruncated;
    }

    const std::uint32_t magic =
        static_cast<std::uint32_t>(segment[0]) |
        (static_cast<std::uint32_t>(segment[1]) << 8) |
        (static_cast<std::uint32_t>(segment[2]) << 16) |
        (static_cast<std::uint32_t>(segment[3]) << 24);

    if (magic == kSessionMagic) {
      SessionWireFrame frame{};
      std::size_t consumed = 0;
      Status decode_st =
          DecodeSessionWireFrame(segment, remaining, &frame, &consumed);
      if (decode_st != Status::kOk) {
        return decode_st;
      }

      Status frame_st = ProcessDecodedSessionFrame(&merger, &frame);
      if (frame_st != Status::kOk) {
        return frame_st;
      }

      offset += consumed;
      continue;
    }

    if (magic == kBatchMagic) {
      wire::BatchDecodeResult decoded = batch_codec.DecodeBatch(segment, remaining);
      if (decoded.status != Status::kOk) {
        return decoded.status;
      }
      if (decoded.consumed_bytes == 0) {
        return Status::kTruncated;
      }

      MergeDeferredLedger& ledger = MergeDeferredLedger::Global();
      if (ledger.HasPending(decoded.frame.header.desk_id)) {
        Status mutation_st =
            ledger.ApplyMutation(decoded.frame.header.desk_id);
        if (mutation_st != Status::kOk) {
          return mutation_st;
        }
      }

      offset += decoded.consumed_bytes;
      continue;
    }

    return Status::kUnknownFormat;
  }

  return Status::kOk;
}

}  // namespace engine
}  // namespace tkr
