package com.tkr.engine;

import com.tkr.nativelink.NativeBridge;
import com.tkr.types.WireTypes.Status;
import com.tkr.types.WireTypes.MergeSlot;
import com.tkr.util.DigestUtil;

import java.util.List;

/** Session merge-slot digest flush via NativeBridge.nativeFlushMergeDigest. */
public final class MergeDigestEngine {

    public static final class MergeDigestResult {
        public Status status = Status.OK;
        public int digest;
        public int slotsProcessed;
    }

    private List<MergeSlot> registered = List.of();

    public void registerMergeSlots(List<MergeSlot> slots) {
        registered = slots != null ? slots : List.of();
        NativeBridge.registerMergeSlotsFromJava(registered);
    }

    public MergeDigestResult flushMergeDigest() {
        MergeDigestResult result = new MergeDigestResult();
        try {
            result.digest = NativeBridge.nativeFlushMergeDigest();
        } catch (UnsatisfiedLinkError e) {
            result.digest = fallbackDigest(registered);
        }
        result.slotsProcessed = registered.size();
        result.status = Status.OK;
        return result;
    }

    private int fallbackDigest(List<MergeSlot> slots) {
        int hash = DigestUtil.FNV_OFFSET;
        for (MergeSlot s : slots) {
            if (s == null || !s.pinned) continue;
            hash = DigestUtil.mixDigest(hash, s.legId);
            if (s.refData != null && s.refLen > 0) {
                hash = DigestUtil.mixDigest(hash, DigestUtil.fnv1a32(s.refData, 0, s.refLen));
            }
        }
        return hash;
    }
}
