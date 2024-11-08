package com.tkr.wire;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.util.BoundsUtil;
import com.tkr.util.DigestUtil;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** TKR3 session wire encode/decode per FORMAT.md. */
public final class SessionWireCodec {

    public static final int SESSION_HEADER_SIZE = 24;
    public static final int SESSION_LEG_SIZE = 28;

    public static final class SessionCodecConfig {
        public boolean verifyMergeDigest = true;
        public int maxLegs = WireTypes.MAX_SESSION_LEGS;
    }

    public static final class SessionDecodeResult {
        public Status status = Status.OK;
        public SessionWireFrame frame = new SessionWireFrame();
        public int bytesConsumed;
    }

    public static final class SessionEncodeResult {
        public Status status = Status.OK;
        public byte[] data = new byte[0];
    }

    private final SessionCodecConfig config;
    private int nextSlotId = 1;

    public SessionWireCodec() { this(new SessionCodecConfig()); }
    public SessionWireCodec(SessionCodecConfig config) {
        this.config = config != null ? config : new SessionCodecConfig();
    }

    public SessionDecodeResult decodeSession(byte[] data, int offset, int length) {
        SessionDecodeResult result = new SessionDecodeResult();
        if (data == null || length < SESSION_HEADER_SIZE) {
            result.status = Status.TRUNCATED;
            return result;
        }
        WireSessionHeader header = new WireSessionHeader();
        readHeader(data, offset, header);
        Status vh = validateHeader(header);
        if (vh != Status.OK) { result.status = vh; return result; }
        result.frame.header = header;
        int legBase = offset + SESSION_HEADER_SIZE;
        List<WireSessionLeg> legs = new ArrayList<>();
        Status lr = readLegs(data, offset + length, legBase, header.legCount, legs);
        if (lr != Status.OK) { result.status = lr; return result; }
        result.frame.legs = legs;
        int refStart = legBase + header.legCount * SESSION_LEG_SIZE;
        byte[] refBlob = Arrays.copyOfRange(data, refStart, offset + length);
        result.frame.refBlob = refBlob;
        for (WireSessionLeg leg : legs) {
            Status rb = validateLegBounds(leg, refBlob.length);
            if (rb != Status.OK) { result.status = rb; return result; }
        }
        if (config.verifyMergeDigest && header.mergeDigest != 0) {
            int computed = computeRefDigest(refBlob);
            if (computed != header.mergeDigest) {
                result.status = Status.BOUNDS_ERROR;
                return result;
            }
        }
        buildMergeSlots(result.frame);
        result.bytesConsumed = length;
        return result;
    }

    public SessionDecodeResult decodeSession(byte[] data) {
        return decodeSession(data, 0, data != null ? data.length : 0);
    }

    public SessionEncodeResult encodeSession(SessionWireFrame frame) {
        SessionEncodeResult result = new SessionEncodeResult();
        if (frame == null || frame.legs.size() > config.maxLegs) {
            result.status = Status.BOUNDS_ERROR;
            return result;
        }
        if (frame.refBlob == null) frame.refBlob = new byte[0];
        frame.header.magic = WireTypes.SESSION_MAGIC;
        frame.header.version = WireTypes.WIRE_VERSION;
        frame.header.legCount = frame.legs.size();
        frame.header.mergeDigest = computeRefDigest(frame.refBlob);
        int total = SESSION_HEADER_SIZE + frame.legs.size() * SESSION_LEG_SIZE + frame.refBlob.length;
        byte[] out = new byte[total];
        writeHeader(out, 0, frame.header);
        int cursor = SESSION_HEADER_SIZE;
        for (WireSessionLeg leg : frame.legs) {
            writeLeg(out, cursor, leg);
            cursor += SESSION_LEG_SIZE;
        }
        System.arraycopy(frame.refBlob, 0, out, cursor, frame.refBlob.length);
        result.data = out;
        return result;
    }

    private void readHeader(byte[] data, int offset, WireSessionHeader out) {
        out.magic = BoundsUtil.readU32Le(data, offset);
        out.version = BoundsUtil.readU16Le(data, offset + 4);
        out.legCount = BoundsUtil.readU16Le(data, offset + 6);
        out.sessionId = BoundsUtil.readU32Le(data, offset + 8);
        out.checkpointSeq = BoundsUtil.readU32Le(data, offset + 12);
        out.flags = BoundsUtil.readU32Le(data, offset + 16);
        out.mergeDigest = BoundsUtil.readU32Le(data, offset + 20);
    }

    private void writeHeader(byte[] dst, int offset, WireSessionHeader h) {
        BoundsUtil.writeU32Le(dst, offset, h.magic);
        BoundsUtil.writeU16Le(dst, offset + 4, h.version);
        BoundsUtil.writeU16Le(dst, offset + 6, h.legCount);
        BoundsUtil.writeU32Le(dst, offset + 8, h.sessionId);
        BoundsUtil.writeU32Le(dst, offset + 12, h.checkpointSeq);
        BoundsUtil.writeU32Le(dst, offset + 16, h.flags);
        BoundsUtil.writeU32Le(dst, offset + 20, h.mergeDigest);
    }

    private Status readLegs(byte[] data, int totalSize, int offset, int count, List<WireSessionLeg> out) {
        if (!BoundsUtil.sectionBodyInBounds(offset, (long) count * SESSION_LEG_SIZE, totalSize)) {
            return Status.BOUNDS_ERROR;
        }
        Set<Integer> seen = new HashSet<>();
        for (int i = 0; i < count; i++) {
            int base = offset + i * SESSION_LEG_SIZE;
            WireSessionLeg leg = new WireSessionLeg();
            leg.legId = BoundsUtil.readU32Le(data, base);
            leg.clOrdId = BoundsUtil.readU32Le(data, base + 4);
            leg.allocAccount = BoundsUtil.readU32Le(data, base + 8);
            leg.qtyMilli = BoundsUtil.readU32Le(data, base + 12);
            leg.refOffset = BoundsUtil.readU32Le(data, base + 16);
            leg.refLen = BoundsUtil.readU32Le(data, base + 20);
            leg.flags = BoundsUtil.readU32Le(data, base + 24);
            if (!seen.add(leg.legId)) return Status.DUPLICATE_KEY;
            out.add(leg);
        }
        return Status.OK;
    }

    private void writeLeg(byte[] dst, int offset, WireSessionLeg leg) {
        BoundsUtil.writeU32Le(dst, offset, leg.legId);
        BoundsUtil.writeU32Le(dst, offset + 4, leg.clOrdId);
        BoundsUtil.writeU32Le(dst, offset + 8, leg.allocAccount);
        BoundsUtil.writeU32Le(dst, offset + 12, leg.qtyMilli);
        BoundsUtil.writeU32Le(dst, offset + 16, leg.refOffset);
        BoundsUtil.writeU32Le(dst, offset + 20, leg.refLen);
        BoundsUtil.writeU32Le(dst, offset + 24, leg.flags);
    }

    private Status validateHeader(WireSessionHeader header) {
        if (header.magic != WireTypes.SESSION_MAGIC) return Status.INVALID_MAGIC;
        if (header.version != WireTypes.WIRE_VERSION) return Status.BOUNDS_ERROR;
        if (header.legCount > config.maxLegs) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    private Status validateLegBounds(WireSessionLeg leg, int refSize) {
        if (leg.refLen == 0) return Status.OK;
        return BoundsUtil.sliceInBounds(leg.refOffset, leg.refLen, refSize) ? Status.OK : Status.BOUNDS_ERROR;
    }

    private void buildMergeSlots(SessionWireFrame frame) {
        frame.mergeSlots.clear();
        for (WireSessionLeg leg : frame.legs) {
            if ((leg.flags & WireTypes.LEG_FLAG_MERGE_PENDING) == 0 || leg.refLen == 0) continue;
            MergeSlot slot = new MergeSlot();
            slot.slotId = nextSlotId++;
            slot.legId = leg.legId;
            slot.refLen = leg.refLen;
            slot.refData = Arrays.copyOfRange(frame.refBlob, leg.refOffset, leg.refOffset + leg.refLen);
            slot.checkpointSeq = frame.header.checkpointSeq;
            slot.pinned = false;
            frame.mergeSlots.add(slot);
        }
    }

    public int computeRefDigest(byte[] refBlob) {
        return DigestUtil.fnv1a32(refBlob);
    }

    public int computeLegTableDigest(List<WireSessionLeg> legs) {
        int hash = DigestUtil.FNV_OFFSET;
        for (WireSessionLeg leg : legs) {
            hash = DigestUtil.mixDigest(hash, leg.legId);
            hash = DigestUtil.mixDigest(hash, leg.allocAccount);
            hash = DigestUtil.mixDigest(hash, leg.qtyMilli);
        }
        return hash;
    }

    public long sumQtyMilli(SessionWireFrame frame) {
        long sum = 0;
        for (WireSessionLeg leg : frame.legs) sum += leg.qtyMilli;
        return sum;
    }

    public int computeHeaderDigest(WireSessionHeader header) {
        byte[] buf = new byte[SESSION_HEADER_SIZE];
        BoundsUtil.writeU32Le(buf, 0, header.magic);
        BoundsUtil.writeU16Le(buf, 4, header.version);
        BoundsUtil.writeU16Le(buf, 6, header.legCount);
        BoundsUtil.writeU32Le(buf, 8, header.sessionId);
        BoundsUtil.writeU32Le(buf, 12, header.checkpointSeq);
        BoundsUtil.writeU32Le(buf, 16, header.flags);
        BoundsUtil.writeU32Le(buf, 20, 0);
        return DigestUtil.fnv1a32(buf, 0, SESSION_HEADER_SIZE);
    }

    public Status stageMergeSlots(SessionWireFrame frame) {
        if (frame == null) return Status.BOUNDS_ERROR;
        frame.mergeSlots.clear();
        nextSlotId = 1;
        for (WireSessionLeg leg : frame.legs) {
            if (leg.refLen == 0) continue;
            if (!BoundsUtil.sliceInBounds(leg.refOffset, leg.refLen, frame.refBlob.length)) return Status.BOUNDS_ERROR;
            MergeSlot slot = new MergeSlot();
            slot.slotId = nextSlotId++;
            slot.legId = leg.legId;
            slot.refLen = leg.refLen;
            slot.refData = Arrays.copyOfRange(frame.refBlob, leg.refOffset, leg.refOffset + leg.refLen);
            slot.checkpointSeq = frame.header.checkpointSeq;
            slot.pinned = (leg.flags & WireTypes.LEG_FLAG_CHECKPOINT_PIN) != 0;
            frame.mergeSlots.add(slot);
            if (frame.mergeSlots.size() > WireTypes.MAX_DEFERRED_SLOTS) return Status.BOUNDS_ERROR;
        }
        return Status.OK;
    }

    public Status clearMergeSlots(SessionWireFrame frame) {
        if (frame == null) return Status.BOUNDS_ERROR;
        for (MergeSlot slot : frame.mergeSlots) {
            slot.refData = null;
            slot.pinned = false;
        }
        frame.mergeSlots.clear();
        return Status.OK;
    }

    public SessionEncodeResult encodeLegsOnly(List<WireSessionLeg> legs, byte[] refBlob) {
        WireSessionHeader header = new WireSessionHeader();
        header.magic = WireTypes.SESSION_MAGIC;
        header.version = WireTypes.WIRE_VERSION;
        header.legCount = legs.size();
        header.sessionId = 1;
        header.checkpointSeq = 1;
        header.flags = WireTypes.LEG_FLAG_MERGE_PENDING;
        SessionWireFrame frame = new SessionWireFrame();
        frame.header = header;
        frame.legs = legs;
        frame.refBlob = refBlob != null ? refBlob : new byte[0];
        return encodeSession(frame);
    }

    public Status patchLegRefOffset(WireSessionLeg leg, int newOffset) {
        if (leg == null) return Status.BOUNDS_ERROR;
        leg.refOffset = newOffset;
        return Status.OK;
    }

    public Status relocateRefBlob(SessionWireFrame frame) {
        if (frame == null) return Status.BOUNDS_ERROR;
        byte[] compact = new byte[0];
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        for (WireSessionLeg leg : frame.legs) {
            if (leg.refLen == 0) {
                leg.refOffset = out.size();
                continue;
            }
            if (!BoundsUtil.sliceInBounds(leg.refOffset, leg.refLen, frame.refBlob.length)) return Status.BOUNDS_ERROR;
            int oldOffset = leg.refOffset;
            leg.refOffset = out.size();
            out.writeBytes(Arrays.copyOfRange(frame.refBlob, oldOffset, oldOffset + leg.refLen));
        }
        frame.refBlob = out.toByteArray();
        return Status.OK;
    }

    public SessionDecodeResult decodePartial(byte[] data, int offset, int length) {
        SessionDecodeResult result = new SessionDecodeResult();
        if (length < SESSION_HEADER_SIZE + SESSION_LEG_SIZE) {
            result.status = Status.PARTIAL_FRAME;
            return result;
        }
        return decodeSession(data, offset, length);
    }

    public static int estimateEncodedSize(int legCount, int refSize) {
        return SESSION_HEADER_SIZE + legCount * SESSION_LEG_SIZE + refSize;
    }

    public Status validateLegRefs(SessionWireFrame frame) {
        for (WireSessionLeg leg : frame.legs) {
            Status st = validateLegBounds(leg, frame.refBlob.length);
            if (st != Status.OK) return st;
        }
        return Status.OK;
    }

    private void appendLegBytes(java.io.ByteArrayOutputStream out, WireSessionLeg leg) throws java.io.IOException {
        byte[] buf = new byte[4];
        BoundsUtil.writeU32Le(buf, 0, leg.legId);
        out.write(buf);
        BoundsUtil.writeU32Le(buf, 0, leg.clOrdId);
        out.write(buf);
        BoundsUtil.writeU32Le(buf, 0, leg.allocAccount);
        out.write(buf);
        BoundsUtil.writeU32Le(buf, 0, leg.qtyMilli);
        out.write(buf);
        BoundsUtil.writeU32Le(buf, 0, leg.refOffset);
        out.write(buf);
        BoundsUtil.writeU32Le(buf, 0, leg.refLen);
        out.write(buf);
        BoundsUtil.writeU32Le(buf, 0, leg.flags);
        out.write(buf);
    }
// --- expanded helpers ---
    public SessionWireFrame createMinimalFrame(int sessionId, int legCount) {
        SessionWireFrame frame = new SessionWireFrame();
        frame.header.sessionId = sessionId;
        for (int i = 0; i < legCount; i++) {
            WireSessionLeg leg = new WireSessionLeg();
            leg.legId = i + 1;
            leg.qtyMilli = 1000;
            frame.legs.add(leg);
        }
        return frame;
    }
}
