#include "tkr/engine/pipeline_orchestrator.h"

#include "tkr/desk/desk_registry.h"
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

Status ProcessBatchFrame(BatchWireFrame* frame) {
  if (frame == nullptr) {
    return Status::kBoundsError;
  }

  desk::DeskRegistry registry;
  desk::DeskRunContext ctx{};
  ctx.batch_id = frame->header.desk_id;
  ctx.record_count = static_cast<std::uint32_t>(frame->records.size());
  Status desk_st = registry.RunBatchDesks(*frame, &ctx);
  if (desk_st != Status::kOk) {
    return desk_st;
  }
  ++g_stats.desk_calls;

  BatchDigestEngine digest;
  digest.RegisterDeferredSlots(frame->deferred_slots);

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

}  // namespace

const PipelineStats& LastPipelineStats() { return g_stats; }

Status RunAllocationPipeline(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len == 0) {
    return Status::kTruncated;
  }

  wire::BatchWireCodec codec(wire::BatchCodecConfig{true, true, kMaxBatchRecords});
  wire::BatchDecodeResult decoded = codec.DecodeBatch(data, len);
  if (decoded.status != Status::kOk) {
    return decoded.status;
  }

  if ((decoded.frame.header.flags & kBatchFlagDeferredDigest) == 0) {
    return Status::kOk;
  }

  wire::WireValidator validator(true, kMaxBatchRecords);
  wire::ValidationResult validated =
      validator.ValidateBatchFrame(decoded.frame);
  if (validated.status != Status::kOk) {
    return validated.status;
  }

  return ProcessBatchFrame(&decoded.frame);
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
