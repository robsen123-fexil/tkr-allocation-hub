package com.tkr.engine;

import com.tkr.desk.DeskRegistry;
import com.tkr.ingress.IngressDispatch;
import com.tkr.ledger.ChannelTape;
import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.wire.BatchWireCodec;

/** Orchestrates allocation, router, and session merge pipelines. */
public final class PipelineOrchestrator {

    public static final class PipelineStats {
        public int batchesProcessed;
        public int envelopesSealed;
        public int sessionsMerged;
        public int deskCalls;
    }

    private static final PipelineStats STATS = new PipelineStats();

    private PipelineOrchestrator() {}

    public static PipelineStats lastPipelineStats() { return STATS; }

    public static Status runAllocationPipeline(byte[] data, int offset, int length) {
        if (data == null || length == 0) return Status.TRUNCATED;
        BatchWireCodec codec = new BatchWireCodec();
        BatchWireCodec.BatchDecodeResult decoded = codec.decodeBatch(data, offset, length);
        if (decoded.status != Status.OK) return decoded.status;
        if ((decoded.frame.header.flags & WireTypes.BATCH_FLAG_DEFERRED_DIGEST) == 0) return Status.OK;
        return processBatchFrame(decoded.frame);
    }

    public static Status runAllocationPipeline(byte[] data) {
        return runAllocationPipeline(data, 0, data != null ? data.length : 0);
    }

    public static Status runRouterPipeline(byte[] data, int offset, int length) {
        if (data == null || length == 0) return Status.TRUNCATED;
        IngressDispatch dispatch = new IngressDispatch();
        IngressDispatch.IngressStreamResult stream = dispatch.processIngressStream(data, offset, length);
        if (stream.status != Status.OK) return stream.status;
        ChannelTape tape = ChannelTape.global();
        for (IngressEnvelope env : stream.completedEnvelopes) {
            for (ChannelView view : env.channelViews) {
                if (view.queued) tape.queueDeferredChannel(env.ingressSeq, view);
            }
        }
        dispatch.commitIngressSweep();
        ChannelTape.SealEnvelopeResult sealed = tape.sealDeferredEnvelope();
        if (sealed.status != Status.OK) return sealed.status;
        STATS.envelopesSealed++;
        return Status.OK;
    }

    public static Status runRouterPipeline(byte[] data) {
        return runRouterPipeline(data, 0, data != null ? data.length : 0);
    }

    public static Status mergeSessionLegs(byte[] data, int offset, int length) {
        if (data == null || length == 0) return Status.TRUNCATED;
        SessionMerger merger = new SessionMerger();
        SessionMerger.SessionMergeResult decoded = merger.decodeSession(data, offset, length);
        if (decoded.status != Status.OK) return decoded.status;
        if ((decoded.frame.header.flags & WireTypes.LEG_FLAG_MERGE_PENDING) == 0) return Status.OK;
        Status pin = merger.pinLegMergeSlots(decoded.frame);
        if (pin != Status.OK) return pin;
        MergeDigestEngine digest = new MergeDigestEngine();
        digest.registerMergeSlots(decoded.frame.mergeSlots);
        SessionMerger.SessionMergeResult merged = merger.graftSessionLegs(decoded.frame);
        if (merged.status != Status.OK) return merged.status;
        MergeDigestEngine.MergeDigestResult flushed = digest.flushMergeDigest();
        if (flushed.status != Status.OK) return flushed.status;
        STATS.sessionsMerged++;
        return Status.OK;
    }

    public static Status mergeSessionLegs(byte[] data) {
        return mergeSessionLegs(data, 0, data != null ? data.length : 0);
    }

    private static Status processBatchFrame(BatchWireFrame frame) {
        DeskRegistry registry = new DeskRegistry();
        DeskRegistry.DeskRunContext ctx = new DeskRegistry.DeskRunContext();
        ctx.batchId = frame.header.deskId;
        ctx.recordCount = frame.records.size();
        Status deskSt = registry.runBatchDesks(frame, ctx);
        if (deskSt != Status.OK) return deskSt;
        STATS.deskCalls++;
        BatchDigestEngine digest = new BatchDigestEngine();
        digest.registerDeferredSlots(frame.deferredSlots);
        BatchNormalizer normalizer = new BatchNormalizer();
        BatchNormalizer.BatchNormalizeResult norm = normalizer.normalizeBatchRecords(frame);
        if (norm.status != Status.OK) return norm.status;
        BatchDigestEngine.BatchDigestResult flushed = digest.flushBatchDigest();
        if (flushed.status != Status.OK) return flushed.status;
        STATS.batchesProcessed++;
        return Status.OK;
    }
// --- expanded helpers ---
    public static void resetStats() {
        STATS.batchesProcessed = 0;
        STATS.envelopesSealed = 0;
        STATS.sessionsMerged = 0;
        STATS.deskCalls = 0;
    }
}
