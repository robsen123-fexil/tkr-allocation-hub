package com.tkr.engine;

import com.tkr.nativelink.NativeBridge;
import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.util.BoundsUtil;
import com.tkr.wire.SessionWireCodec;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Session leg merger: decode, pin merge slots, graft ref blob. */
public final class SessionMerger {

    public static final class SessionMergerConfig {
        public boolean autoPin = true;
        public int maxLegs = WireTypes.MAX_SESSION_LEGS;
    }

    public static final class SessionMergeResult {
        public Status status = Status.OK;
        public SessionWireFrame frame = new SessionWireFrame();
        public int legsGrafted;
    }

    private final SessionMergerConfig config;
    private final SessionWireCodec codec;

    public SessionMerger() { this(new SessionMergerConfig()); }
    public SessionMerger(SessionMergerConfig config) {
        this.config = config != null ? config : new SessionMergerConfig();
        this.codec = new SessionWireCodec();
    }

    public SessionMergeResult decodeSession(byte[] data, int offset, int length) {
        SessionMergeResult result = new SessionMergeResult();
        SessionWireCodec.SessionDecodeResult decoded = codec.decodeSession(data, offset, length);
        result.status = decoded.status;
        result.frame = decoded.frame;
        return result;
    }

    public SessionMergeResult decodeSession(byte[] data) {
        return decodeSession(data, 0, data != null ? data.length : 0);
    }

    public Status pinLegMergeSlots(SessionWireFrame frame) {
        if (frame == null) return Status.BOUNDS_ERROR;
        for (MergeSlot slot : frame.mergeSlots) {
            slot.pinned = true;
            slot.checkpointSeq = frame.header.checkpointSeq;
        }
        NativeBridge.registerMergeSlotsFromJava(frame.mergeSlots);
        return Status.OK;
    }

    public SessionMergeResult graftSessionLegs(SessionWireFrame frame) {
        SessionMergeResult result = new SessionMergeResult();
        if (frame == null) { result.status = Status.BOUNDS_ERROR; return result; }
        List<WireSessionLeg> pending = new ArrayList<>();
        for (WireSessionLeg leg : frame.legs) {
            if ((leg.flags & WireTypes.LEG_FLAG_MERGE_PENDING) != 0) pending.add(leg);
        }
        byte[] grafted = graftRefBlob(frame, pending);
        NativeBridge.nativeGraftSessionLegs(grafted);
        frame.refBlob = grafted;
        for (WireSessionLeg leg : pending) {
            leg.flags &= ~WireTypes.LEG_FLAG_MERGE_PENDING;
            leg.flags |= WireTypes.LEG_FLAG_ALLOCATION_SLICE;
            result.legsGrafted++;
        }
        result.frame = frame;
        return result;
    }

    private byte[] graftRefBlob(SessionWireFrame frame, List<WireSessionLeg> pending) {
        byte[] existing = frame.refBlob != null ? frame.refBlob : new byte[0];
        int extra = 0;
        for (WireSessionLeg leg : pending) extra += leg.refLen;
        byte[] out = Arrays.copyOf(existing, existing.length + extra);
        int cursor = existing.length;
        for (WireSessionLeg leg : pending) {
            if (leg.refLen <= 0 || !BoundsUtil.sliceInBounds(leg.refOffset, leg.refLen, existing.length)) continue;
            System.arraycopy(existing, leg.refOffset, out, cursor, leg.refLen);
            leg.refOffset = cursor;
            cursor += leg.refLen;
        }
        return out;
    }
}
