package com.tkr.ingress;

import com.tkr.nativelink.NativeBridge;
import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.util.BoundsUtil;
import com.tkr.util.DigestUtil;
import com.tkr.wire.BatchWireCodec;
import com.tkr.wire.SessionWireCodec;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Dispatches classified ingress streams into envelope channel views. */
public final class IngressDispatch {

    public static final int ENVELOPE_HEADER_SIZE = 24;
    public static final int ENVELOPE_CHANNEL_SIZE = 20;

    public static final class IngressDispatchConfig {
        public boolean sealOnSweep = true;
        public boolean queueDeferred = true;
        public boolean validateDigests = true;
        public int maxChannels = WireTypes.MAX_ENVELOPE_CHANNELS;
    }

    public static final class IngressStreamResult {
        public Status status = Status.OK;
        public List<IngressEnvelope> completedEnvelopes = new ArrayList<>();
        public int framesParsed;
    }

    private final IngressDispatchConfig config;
    private final IngressClassifier classifier;
    private int nextIngressSeq = 1;

    public IngressDispatch() { this(new IngressDispatchConfig()); }
    public IngressDispatch(IngressDispatchConfig config) {
        this.config = config != null ? config : new IngressDispatchConfig();
        this.classifier = new IngressClassifier();
    }

    public IngressStreamResult processIngressStream(byte[] data, int offset, int length) {
        IngressStreamResult result = new IngressStreamResult();
        if (data == null || length <= 0) { result.status = Status.TRUNCATED; return result; }
        IngressKind kind = classifier.classify(data, offset, length);
        switch (kind) {
            case ENVELOPE_WIRE -> {
                Status st = parseEnvelopeFrame(data, offset, length, result);
                if (st != Status.OK) result.status = st;
            }
            case BATCH_WIRE -> wrapBatchAsEnvelope(data, offset, length, result);
            case SESSION_WIRE -> wrapSessionAsEnvelope(data, offset, length, result);
            case FIX44, SWIFT_MT940 -> wrapTextAsEnvelope(data, offset, length, kind, result);
            default -> result.status = Status.UNKNOWN_FORMAT;
        }
        return result;
    }

    public IngressStreamResult processIngressStream(byte[] data) {
        return processIngressStream(data, 0, data != null ? data.length : 0);
    }

    public void commitIngressSweep() {
        NativeBridge.nativeCommitIngressSweep();
    }

    private Status parseEnvelopeFrame(byte[] data, int offset, int length, IngressStreamResult result) {
        if (length < ENVELOPE_HEADER_SIZE) return Status.TRUNCATED;
        WireEnvelopeHeader header = new WireEnvelopeHeader();
        header.magic = BoundsUtil.readU32Le(data, offset);
        header.version = BoundsUtil.readU16Le(data, offset + 4);
        header.channelCount = BoundsUtil.readU16Le(data, offset + 6);
        header.flags = BoundsUtil.readU32Le(data, offset + 8);
        header.ingressSeq = BoundsUtil.readU32Le(data, offset + 12);
        header.parentBatchId = BoundsUtil.readU32Le(data, offset + 16);
        header.sealDigest = BoundsUtil.readU32Le(data, offset + 20);
        if (header.magic != WireTypes.ENVELOPE_MAGIC) return Status.INVALID_MAGIC;
        if (header.channelCount > config.maxChannels) return Status.BOUNDS_ERROR;
        int channelBase = offset + ENVELOPE_HEADER_SIZE;
        long need = (long) header.channelCount * ENVELOPE_CHANNEL_SIZE;
        if (!BoundsUtil.sectionBodyInBounds(channelBase, need, offset + length)) return Status.BOUNDS_ERROR;
        List<WireEnvelopeChannel> channels = new ArrayList<>();
        int payloadStart = channelBase + (int) need;
        byte[] ownedPayload = Arrays.copyOfRange(data, payloadStart, offset + length);
        for (int i = 0; i < header.channelCount; i++) {
            int base = channelBase + i * ENVELOPE_CHANNEL_SIZE;
            WireEnvelopeChannel ch = new WireEnvelopeChannel();
            ch.channelId = BoundsUtil.readU32Le(data, base);
            ch.kind = BoundsUtil.readU32Le(data, base + 4);
            ch.payloadOffset = BoundsUtil.readU32Le(data, base + 8);
            ch.payloadLen = BoundsUtil.readU32Le(data, base + 12);
            ch.routeHint = BoundsUtil.readU32Le(data, base + 16);
            channels.add(ch);
        }
        if (config.validateDigests && header.sealDigest != 0) {
            if (DigestUtil.fnv1a32(ownedPayload) != header.sealDigest) return Status.BOUNDS_ERROR;
        }
        IngressEnvelope env = buildEnvelope(header, channels, ownedPayload);
        result.completedEnvelopes.add(env);
        result.framesParsed++;
        return Status.OK;
    }

    private IngressEnvelope buildEnvelope(WireEnvelopeHeader header, List<WireEnvelopeChannel> channels, byte[] payload) {
        IngressEnvelope env = new IngressEnvelope();
        env.wire.header = header;
        env.wire.channels = channels;
        env.ownedPayload = payload;
        env.ingressSeq = header.ingressSeq != 0 ? header.ingressSeq : nextIngressSeq++;
        env.sourceKind = IngressKind.ENVELOPE_WIRE;
        for (WireEnvelopeChannel ch : channels) {
            ChannelView view = new ChannelView();
            view.channelId = ch.channelId;
            view.kind = ch.kind;
            view.routeHint = ch.routeHint;
            view.payloadLen = ch.payloadLen;
            if (ch.payloadLen > 0 && BoundsUtil.sliceInBounds(ch.payloadOffset, ch.payloadLen, payload.length)) {
                view.payload = Arrays.copyOfRange(payload, ch.payloadOffset, ch.payloadOffset + ch.payloadLen);
            } else {
                view.payload = new byte[0];
            }
            view.queued = config.queueDeferred && (header.flags & WireTypes.ENVELOPE_FLAG_SEAL_PENDING) != 0;
            env.channelViews.add(view);
            if (view.queued && view.payloadLen > 0) {
                NativeBridge.nativeQueueChannelEntry(0, view.payloadLen);
            }
        }
        return env;
    }

    private void wrapBatchAsEnvelope(byte[] data, int offset, int length, IngressStreamResult result) {
        BatchWireCodec codec = new BatchWireCodec();
        BatchWireCodec.BatchDecodeResult decoded = codec.decodeBatch(data, offset, length);
        if (decoded.status != Status.OK) { result.status = decoded.status; return; }
        WireEnvelopeHeader header = new WireEnvelopeHeader();
        header.magic = WireTypes.ENVELOPE_MAGIC;
        header.version = WireTypes.WIRE_VERSION;
        header.channelCount = 1;
        header.flags = WireTypes.ENVELOPE_FLAG_NESTED_BATCH;
        header.parentBatchId = decoded.frame.header.deskId;
        header.sealDigest = decoded.frame.header.payloadDigest;
        WireEnvelopeChannel ch = new WireEnvelopeChannel();
        ch.channelId = 1;
        ch.kind = IngressKind.BATCH_WIRE.ordinal();
        ch.payloadLen = length;
        result.completedEnvelopes.add(buildEnvelope(header, List.of(ch), Arrays.copyOfRange(data, offset, offset + length)));
        result.framesParsed++;
    }

    private void wrapSessionAsEnvelope(byte[] data, int offset, int length, IngressStreamResult result) {
        SessionWireCodec codec = new SessionWireCodec();
        SessionWireCodec.SessionDecodeResult decoded = codec.decodeSession(data, offset, length);
        if (decoded.status != Status.OK) { result.status = decoded.status; return; }
        WireEnvelopeHeader header = new WireEnvelopeHeader();
        header.magic = WireTypes.ENVELOPE_MAGIC;
        header.version = WireTypes.WIRE_VERSION;
        header.channelCount = 1;
        header.flags = WireTypes.ENVELOPE_FLAG_INGRESS_SWEEP;
        header.sealDigest = decoded.frame.header.mergeDigest;
        WireEnvelopeChannel ch = new WireEnvelopeChannel();
        ch.channelId = decoded.frame.header.sessionId;
        ch.kind = IngressKind.SESSION_WIRE.ordinal();
        ch.payloadLen = length;
        result.completedEnvelopes.add(buildEnvelope(header, List.of(ch), Arrays.copyOfRange(data, offset, offset + length)));
        result.framesParsed++;
    }

    private void wrapTextAsEnvelope(byte[] data, int offset, int length, IngressKind kind, IngressStreamResult result) {
        WireEnvelopeHeader header = new WireEnvelopeHeader();
        header.magic = WireTypes.ENVELOPE_MAGIC;
        header.version = WireTypes.WIRE_VERSION;
        header.channelCount = 1;
        header.flags = WireTypes.ENVELOPE_FLAG_CHANNEL_TAPE;
        header.sealDigest = DigestUtil.fnv1a32(data, offset, length);
        WireEnvelopeChannel ch = new WireEnvelopeChannel();
        ch.channelId = nextIngressSeq;
        ch.kind = kind.ordinal();
        ch.payloadLen = length;
        result.completedEnvelopes.add(buildEnvelope(header, List.of(ch), Arrays.copyOfRange(data, offset, offset + length)));
        result.framesParsed++;
    }
// --- expanded helpers ---
    public int envelopeCount(IngressStreamResult result) {
        return result.completedEnvelopes.size();
    }

    public void reset() {
        nextIngressSeq = 0;
    }

    public Status attachChannelViews(IngressEnvelope envelope) {
        if (envelope == null) return Status.BOUNDS_ERROR;
        envelope.channelViews.clear();
        for (WireEnvelopeChannel ch : envelope.wire.channels) {
            if (ch.payloadLen == 0) continue;
            if (!BoundsUtil.sliceInBounds(ch.payloadOffset, ch.payloadLen, envelope.ownedPayload.length)) {
                return Status.BOUNDS_ERROR;
            }
            ChannelView view = new ChannelView();
            view.channelId = ch.channelId;
            view.kind = ch.kind;
            view.payloadLen = ch.payloadLen;
            view.routeHint = ch.routeHint;
            view.payload = Arrays.copyOfRange(envelope.ownedPayload, ch.payloadOffset, ch.payloadOffset + ch.payloadLen);
            view.queued = false;
            envelope.channelViews.add(view);
        }
        return Status.OK;
    }

    public Status queueEnvelopeChannelViews(IngressEnvelope envelope) {
        if (envelope == null) return Status.BOUNDS_ERROR;
        if (!config.queueDeferred) return Status.OK;
        for (ChannelView view : envelope.channelViews) view.queued = true;
        return Status.OK;
    }

    public Status sealPendingEnvelopes(List<IngressEnvelope> pending) {
        for (IngressEnvelope envelope : pending) {
            int seal = DigestUtil.FNV_OFFSET;
            for (ChannelView view : envelope.channelViews) {
                if (view.payload != null && view.payloadLen > 0) {
                    seal = DigestUtil.mixDigest(DigestUtil.fnv1a32(view.payload, 0, view.payloadLen), seal);
                }
            }
            envelope.wire.header.sealDigest = seal;
        }
        return Status.OK;
    }

    public IngressStreamResult processMixedStream(byte[] data, int offset, int length) {
        IngressStreamResult result = new IngressStreamResult();
        String view = new String(data, offset, length, java.nio.charset.StandardCharsets.US_ASCII);
        Status fixSt = dispatchFixSegment(view, result);
        if (fixSt != Status.OK) {
            Status swiftSt = dispatchSwiftSegment(view, result);
            if (swiftSt != Status.OK) {
                result.status = dispatchBatchSegment(data, offset, length, result);
            }
        }
        return result;
    }

    private Status dispatchFixSegment(String segment, IngressStreamResult out) {
        if (segment == null || !segment.startsWith("8=FIX")) return Status.UNKNOWN_FORMAT;
        out.framesParsed++;
        IngressEnvelope envelope = new IngressEnvelope();
        envelope.sourceKind = IngressKind.FIX44;
        envelope.ingressSeq = ++nextIngressSeq;
        envelope.wire.header.magic = WireTypes.ENVELOPE_MAGIC;
        envelope.wire.header.version = WireTypes.WIRE_VERSION;
        envelope.wire.header.flags = WireTypes.ENVELOPE_FLAG_INGRESS_SWEEP;
        envelope.wire.header.ingressSeq = envelope.ingressSeq;
        envelope.ownedPayload = segment.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        WireEnvelopeChannel ch = new WireEnvelopeChannel();
        ch.channelId = envelope.ingressSeq;
        ch.kind = IngressKind.FIX44.ordinal();
        ch.payloadOffset = 0;
        ch.payloadLen = envelope.ownedPayload.length;
        envelope.wire.channels.add(ch);
        Status attach = attachChannelViews(envelope);
        if (attach != Status.OK) return attach;
        queueEnvelopeChannelViews(envelope);
        out.completedEnvelopes.add(envelope);
        return Status.OK;
    }

    private Status dispatchSwiftSegment(String segment, IngressStreamResult out) {
        if (segment == null || !segment.contains(":20:")) return Status.UNKNOWN_FORMAT;
        out.framesParsed++;
        IngressEnvelope envelope = new IngressEnvelope();
        envelope.sourceKind = IngressKind.SWIFT_MT940;
        envelope.ingressSeq = ++nextIngressSeq;
        envelope.wire.header.magic = WireTypes.ENVELOPE_MAGIC;
        envelope.wire.header.version = WireTypes.WIRE_VERSION;
        envelope.wire.header.flags = WireTypes.ENVELOPE_FLAG_CHANNEL_TAPE;
        envelope.wire.header.ingressSeq = envelope.ingressSeq;
        envelope.ownedPayload = segment.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        WireEnvelopeChannel ch = new WireEnvelopeChannel();
        ch.channelId = envelope.ingressSeq;
        ch.kind = IngressKind.SWIFT_MT940.ordinal();
        ch.payloadOffset = 0;
        ch.payloadLen = envelope.ownedPayload.length;
        envelope.wire.channels.add(ch);
        Status attach = attachChannelViews(envelope);
        if (attach != Status.OK) return attach;
        queueEnvelopeChannelViews(envelope);
        out.completedEnvelopes.add(envelope);
        return Status.OK;
    }

    private Status dispatchBatchSegment(byte[] data, int offset, int length, IngressStreamResult out) {
        BatchWireCodec codec = new BatchWireCodec();
        BatchWireCodec.BatchDecodeResult decoded = codec.decodeBatch(data, offset, length);
        if (decoded.status != Status.OK) return decoded.status;
        out.framesParsed++;
        wrapBatchAsEnvelope(data, offset, length, out);
        return Status.OK;
    }

    public static final class IngressStreamStats {
        public int framesSeen;
        public int fixMessages;
        public int swiftStatements;
        public int batchesDecoded;
        public int envelopesQueued;
        public int rejectedFrames;
    }

    public IngressStreamStats statsFor(IngressStreamResult result) {
        IngressStreamStats stats = new IngressStreamStats();
        stats.framesSeen = result.framesParsed;
        stats.envelopesQueued = result.completedEnvelopes.size();
        return stats;
    }

    public Status buildEnvelopeFromBatch(BatchWireCodec.BatchDecodeResult batchResult, IngressEnvelope envelope) {
        if (envelope == null || batchResult == null) return Status.BOUNDS_ERROR;
        envelope.wire.header.magic = WireTypes.ENVELOPE_MAGIC;
        envelope.wire.header.version = WireTypes.WIRE_VERSION;
        envelope.wire.header.channelCount = 1;
        envelope.wire.header.flags = WireTypes.ENVELOPE_FLAG_NESTED_BATCH;
        envelope.wire.header.ingressSeq = ++nextIngressSeq;
        envelope.wire.header.parentBatchId = batchResult.frame.header.deskId;
        envelope.wire.header.sealDigest = 0;
        WireEnvelopeChannel ch = new WireEnvelopeChannel();
        ch.channelId = 1;
        ch.kind = IngressKind.BATCH_WIRE.ordinal();
        ch.payloadOffset = 0;
        ch.payloadLen = batchResult.frame.payloadBlob.length;
        ch.routeHint = batchResult.frame.header.flags;
        envelope.wire.channels = List.of(ch);
        envelope.ownedPayload = batchResult.frame.payloadBlob;
        envelope.sourceKind = IngressKind.BATCH_WIRE;
        envelope.ingressSeq = envelope.wire.header.ingressSeq;
        return attachChannelViews(envelope);
    }

    public int totalChannelPayloadBytes(IngressStreamResult result) {
        int sum = 0;
        for (IngressEnvelope env : result.completedEnvelopes) {
            for (ChannelView view : env.channelViews) sum += view.payloadLen;
        }
        return sum;
    }

    public List<ChannelView> flattenChannelViews(IngressStreamResult result) {
        List<ChannelView> flat = new ArrayList<>();
        for (IngressEnvelope env : result.completedEnvelopes) flat.addAll(env.channelViews);
        return flat;
    }

    public Status validateEnvelopeBounds(IngressEnvelope envelope) {
        if (envelope == null) return Status.BOUNDS_ERROR;
        for (WireEnvelopeChannel ch : envelope.wire.channels) {
            if (ch.payloadLen > 0 && !BoundsUtil.sliceInBounds(ch.payloadOffset, ch.payloadLen, envelope.ownedPayload.length)) {
                return Status.BOUNDS_ERROR;
            }
        }
        return Status.OK;
    }

    public IngressEnvelope cloneEnvelope(IngressEnvelope source) {
        IngressEnvelope copy = new IngressEnvelope();
        copy.wire.header = source.wire.header;
        copy.wire.channels = new ArrayList<>(source.wire.channels);
        copy.ownedPayload = Arrays.copyOf(source.ownedPayload, source.ownedPayload.length);
        copy.sourceKind = source.sourceKind;
        copy.ingressSeq = source.ingressSeq;
        attachChannelViews(copy);
        return copy;
    }

    public String summarizeStream(IngressStreamResult result) {
        return "frames=" + result.framesParsed + " envelopes=" + result.completedEnvelopes.size()
                + " status=" + result.status;
    }
}
