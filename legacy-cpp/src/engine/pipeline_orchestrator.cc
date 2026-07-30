#include "tkr/engine/pipeline_orchestrator.h"

#include "tkr/desk/desk_registry.h"
#include "tkr/engine/batch_deferred_ledger.h"
#include "tkr/engine/batch_digest.h"
#include "tkr/engine/batch_normalizer.h"
#include "tkr/engine/merge_digest.h"
#include "tkr/engine/session_merger.h"
#include "tkr/ingress/ingress_dispatch.h"
#include "tkr/ledger/channel_tape.h"
#include "tkr/wire/batch_wire_codec.h"
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
      ledger.HasPending(frame->header.desk_id,
                        frame->header.trade_date_yyyymmdd)) {
    Status successor_st = ProcessCrossFrameSuccessor(frame);
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

  ingress::IngressDispatch dispatch(ingress::IngressDispatchConfig{
      true, true, true, kMaxEnvelopeChannels});

  ingress::IngressStreamResult stream = dispatch.ProcessIngressStream(data, len);
  if (stream.status != Status::kOk) {
    return stream.status;
  }

  ledger::ChannelTape& tape = ledger::GlobalChannelTape();
  for (const IngressEnvelope& env : stream.completed_envelopes) {
    for (const ChannelView& view : env.channel_views) {
      if (view.queued) {
        tape.QueueDeferredChannel(env.ingress_seq, view);
      }
    }
  }

  dispatch.CommitIngressSweep();

  ledger::SealEnvelopeResult sealed = tape.SealDeferredEnvelope();
  if (sealed.status != Status::kOk) {
    return sealed.status;
  }
  ++g_stats.envelopes_sealed;
  return Status::kOk;
}

Status MergeSessionLegs(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return Status::kTruncated;
  }

  SessionMerger merger(SessionMergerConfig{true, kMaxSessionLegs});
  SessionMergeResult decoded = merger.DecodeSession(data, len);
  if (decoded.status != Status::kOk) {
    return decoded.status;
  }

  if ((decoded.frame.header.flags & kLegFlagMergePending) == 0) {
    return Status::kOk;
  }

  Status pin_st = merger.PinLegMergeSlots(&decoded.frame);
  if (pin_st != Status::kOk) {
    return pin_st;
  }

  MergeDigestEngine digest;
  digest.RegisterMergeSlots(decoded.frame.merge_slots);

  SessionMergeResult merged = merger.GraftSessionLegs(&decoded.frame);
  if (merged.status != Status::kOk) {
    return merged.status;
  }

  MergeDigestResult flushed = digest.FlushMergeDigest();
  if (flushed.status != Status::kOk) {
    return flushed.status;
  }
  ++g_stats.sessions_merged;
  return Status::kOk;
}

}  // namespace engine
}  // namespace tkr
