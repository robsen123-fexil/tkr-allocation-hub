package com.tkr.types;

import java.util.ArrayList;
import java.util.List;

/**
 * Wire magics, flags, and frame types for TKR1/TKR2/TKR3 binary formats.
 */
public final class WireTypes {

    private WireTypes() {}

    public static final int BATCH_MAGIC = 0x544B5231;   // TKR1
    public static final int ENVELOPE_MAGIC = 0x544B5232; // TKR2
    public static final int SESSION_MAGIC = 0x544B5233;  // TKR3

    public static final int WIRE_VERSION = 7;
    public static final int MAX_BATCH_RECORDS = 4096;
    public static final int MAX_ENVELOPE_CHANNELS = 256;
    public static final int MAX_SESSION_LEGS = 512;
    public static final int MAX_DEFERRED_SLOTS = 1024;

    public enum Status {
        OK,
        INVALID_MAGIC,
        TRUNCATED,
        BOUNDS_ERROR,
        DUPLICATE_KEY,
        COMPLIANCE_REJECT,
        MARGIN_BREACH,
        UNKNOWN_FORMAT,
        SESSION_GAP,
        PARTIAL_FRAME;

        public static Status fromOrdinal(int o) {
            Status[] values = values();
            if (o < 0 || o >= values.length) {
                return UNKNOWN_FORMAT;
            }
            return values[o];
        }
    }

    public static final int BATCH_FLAG_NONE = 0;
    public static final int BATCH_FLAG_PRO_RATA = 1 << 0;
    public static final int BATCH_FLAG_MARGIN_CHECK = 1 << 1;
    public static final int BATCH_FLAG_COMPLIANCE_HOLD = 1 << 2;
    public static final int BATCH_FLAG_DEFERRED_DIGEST = 1 << 3;
    public static final int BATCH_FLAG_PARTIAL_FILL = 1 << 4;
    public static final int BATCH_FLAG_CROSS_DESK = 1 << 5;

    public static final int ENVELOPE_FLAG_NONE = 0;
    public static final int ENVELOPE_FLAG_NESTED_BATCH = 1 << 0;
    public static final int ENVELOPE_FLAG_CHANNEL_TAPE = 1 << 1;
    public static final int ENVELOPE_FLAG_SEAL_PENDING = 1 << 2;
    public static final int ENVELOPE_FLAG_INGRESS_SWEEP = 1 << 3;
    public static final int ENVELOPE_FLAG_MARGIN_ENVELOPE = 1 << 4;

    public static final int LEG_FLAG_NONE = 0;
    public static final int LEG_FLAG_ALLOCATION_SLICE = 1 << 0;
    public static final int LEG_FLAG_MERGE_PENDING = 1 << 1;
    public static final int LEG_FLAG_COMPLIANCE_CLEARED = 1 << 2;
    public static final int LEG_FLAG_MARGIN_RESERVED = 1 << 3;
    public static final int LEG_FLAG_CHECKPOINT_PIN = 1 << 4;

    public enum IngressKind {
        UNKNOWN,
        FIX44,
        SWIFT_MT940,
        BATCH_WIRE,
        ENVELOPE_WIRE,
        SESSION_WIRE,
        MIXED_STREAM
    }

    public static final class WireBatchHeader {
        public int magic;
        public int version;
        public int headerBytes;
        public int recordCount;
        public int flags;
        public int deskId;
        public int tradeDateYyyymmdd;
        public int payloadDigest;
    }

    public static final class WireBatchRecord {
        public int recordId;
        public int accountId;
        public int symbolId;
        public int qtyMilli;
        public int priceTick;
        public int payloadOffset;
        public int payloadLen;
        public int flags;
    }

    public static final class WireEnvelopeHeader {
        public int magic;
        public int version;
        public int channelCount;
        public int flags;
        public int ingressSeq;
        public int parentBatchId;
        public int sealDigest;
    }

    public static final class WireEnvelopeChannel {
        public int channelId;
        public int kind;
        public int payloadOffset;
        public int payloadLen;
        public int routeHint;
    }

    public static final class WireEnvelope {
        public WireEnvelopeHeader header = new WireEnvelopeHeader();
        public List<WireEnvelopeChannel> channels = new ArrayList<>();
    }

    public static final class WireSessionHeader {
        public int magic;
        public int version;
        public int legCount;
        public int sessionId;
        public int checkpointSeq;
        public int flags;
        public int mergeDigest;
    }

    public static final class WireSessionLeg {
        public int legId;
        public int clOrdId;
        public int allocAccount;
        public int qtyMilli;
        public int refOffset;
        public int refLen;
        public int flags;
    }

    public static final class DeferredSlot {
        public int slotId;
        public int recordId;
        public byte[] payload;
        public int payloadLen;
        public int stagingFlags;
        public boolean active;
    }

    public static final class ChannelView {
        public int channelId;
        public int kind;
        public byte[] payload;
        public int payloadLen;
        public int routeHint;
        public boolean queued;
    }

    public static final class MergeSlot {
        public int slotId;
        public int legId;
        public byte[] refData;
        public int refLen;
        public int checkpointSeq;
        public boolean pinned;
    }

    public static final class BatchWireFrame {
        public WireBatchHeader header = new WireBatchHeader();
        public List<WireBatchRecord> records = new ArrayList<>();
        public byte[] payloadBlob = new byte[0];
        public List<DeferredSlot> deferredSlots = new ArrayList<>();
    }

    public static final class IngressEnvelope {
        public WireEnvelope wire = new WireEnvelope();
        public byte[] ownedPayload = new byte[0];
        public List<ChannelView> channelViews = new ArrayList<>();
        public IngressKind sourceKind = IngressKind.UNKNOWN;
        public int ingressSeq;
    }

    public static final class SessionWireFrame {
        public WireSessionHeader header = new WireSessionHeader();
        public List<WireSessionLeg> legs = new ArrayList<>();
        public byte[] refBlob = new byte[0];
        public List<MergeSlot> mergeSlots = new ArrayList<>();
    }

    public static final class AllocationBatchSummary {
        public int batchId;
        public int recordCount;
        public int totalQtyMilli;
        public int deskId;
        public int flags;
        public boolean marginCleared;
        public boolean complianceCleared;
    }

    public static final class MarginCheckResult {
        public int accountId;
        public long requiredMarginCents;
        public long availableMarginCents;
        public boolean passed;
    }

    public static String statusToString(Status status) {
        if (status == null) {
            return "unknown";
        }
        return switch (status) {
            case OK -> "ok";
            case INVALID_MAGIC -> "invalid_magic";
            case TRUNCATED -> "truncated";
            case BOUNDS_ERROR -> "bounds_error";
            case DUPLICATE_KEY -> "duplicate_key";
            case COMPLIANCE_REJECT -> "compliance_reject";
            case MARGIN_BREACH -> "margin_breach";
            case UNKNOWN_FORMAT -> "unknown_format";
            case SESSION_GAP -> "session_gap";
            case PARTIAL_FRAME -> "partial_frame";
        };
    }
}
