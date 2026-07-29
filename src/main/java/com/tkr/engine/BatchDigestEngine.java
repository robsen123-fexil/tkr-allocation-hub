package com.tkr.engine;

import com.tkr.nativelink.NativeBridge;
import com.tkr.types.WireTypes.Status;
import com.tkr.types.WireTypes.DeferredSlot;
import com.tkr.util.DigestUtil;

import java.util.Arrays;

/** Batch deferred-slot digest flush via NativeBridge.nativeFlushBatchDigest. */
public final class BatchDigestEngine {

    public static final class BatchDigestResult {
        public Status status = Status.OK;
        public int digest;
        public int slotsProcessed;
    }

    private DeferredSlot[] registered = new DeferredSlot[0];
    private byte[] compactPayload = new byte[0];

    public void registerDeferredSlots(java.util.List<DeferredSlot> slots) {
        registered = slots != null ? slots.toArray(new DeferredSlot[0]) : new DeferredSlot[0];
        NativeBridge.registerBatchSlotsFromJava(registered);
    }

    public void registerDeferredSlots(DeferredSlot[] slots) {
        registered = slots != null ? slots : new DeferredSlot[0];
        NativeBridge.registerBatchSlotsFromJava(registered);
    }

    public BatchDigestResult flushBatchDigest() {
        BatchDigestResult result = new BatchDigestResult();
        if (registered.length > 0 && compactPayload.length == 0) {
            compactPayload = compactSlotPayloads(registered);
            if (compactPayload.length > 0) {
                NativeBridge.nativeCompactBatchPayload(compactPayload);
            }
        }
        if (!NativeBridge.isNativeLoaded()) {
            throw new UnsatisfiedLinkError("libtkr_native.so is required for batch digest flush");
        }
        result.digest = NativeBridge.nativeFlushBatchDigest();
        result.slotsProcessed = registered.length;
        result.status = Status.OK;
        return result;
    }

    private byte[] compactSlotPayloads(DeferredSlot[] slots) {
        int total = 0;
        for (DeferredSlot s : slots) {
            if (s != null && s.active && s.payloadLen > 0) total += s.payloadLen;
        }
        byte[] out = new byte[total];
        int cursor = 0;
        for (DeferredSlot s : slots) {
            if (s == null || !s.active || s.payloadLen <= 0 || s.payload == null) continue;
            System.arraycopy(s.payload, 0, out, cursor, s.payloadLen);
            cursor += s.payloadLen;
        }
        return out;
    }

    private int fallbackDigest(DeferredSlot[] slots) {
        int hash = DigestUtil.FNV_OFFSET;
        for (DeferredSlot s : slots) {
            if (s == null || !s.active) continue;
            hash = DigestUtil.mixDigest(hash, s.recordId);
            if (s.payload != null && s.payloadLen > 0) {
                hash = DigestUtil.mixDigest(hash, DigestUtil.fnv1a32(s.payload, 0, s.payloadLen));
            }
        }
        return hash;
    }

    public void reset() {
        registered = new DeferredSlot[0];
        compactPayload = new byte[0];
        try { NativeBridge.nativeResetState(); } catch (UnsatisfiedLinkError ignored) { }
    }
}
