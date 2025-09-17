package com.tkr.ledger;

import com.tkr.nativelink.NativeBridge;
import com.tkr.types.WireTypes.ChannelView;
import com.tkr.types.WireTypes.Status;
import com.tkr.util.DigestUtil;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Channel tape for deferred envelope sealing with sorted digest chain.
 * Ported from legacy-cpp channel_tape.cc.
 */
public final class ChannelTape {

    public static final class ChannelTapeEntry {
        public int envelopeSeq;
        public int channelId;
        public byte[] payload;
        public int payloadLen;
        public boolean sealed;
    }

    public static final class SealEnvelopeResult {
        public Status status = Status.OK;
        public int channelsSealed;
        public int sealDigest;
    }

    private static final class TapeDigestEntry {
        int envelopeSeq;
        int channelId;
        int payloadDigest;
        int payloadLen;
    }

    private static final ChannelTape GLOBAL = new ChannelTape();

    public static ChannelTape global() {
        return GLOBAL;
    }

    private final List<ChannelTapeEntry> entries = new ArrayList<>();
    private int nextSealRound;

    public void queueDeferredChannel(int envelopeSeq, ChannelView view) {
        if (view == null || view.payloadLen <= 0) return;
        ChannelTapeEntry entry = new ChannelTapeEntry();
        entry.envelopeSeq = envelopeSeq;
        entry.channelId = view.channelId;
        entry.payloadLen = view.payloadLen;
        if (view.payload != null && view.payloadLen > 0) {
            entry.payload = new byte[view.payloadLen];
            System.arraycopy(view.payload, 0, entry.payload, 0, view.payloadLen);
        }
        entry.sealed = false;
        entries.add(entry);
    }

    public void queueDeferredChannel(int envelopeSeq, int channelId, byte[] payload, int payloadLen) {
        ChannelView view = new ChannelView();
        view.channelId = channelId;
        view.payload = payload;
        view.payloadLen = payloadLen;
        queueDeferredChannel(envelopeSeq, view);
    }

    public void clearPending() {
        entries.clear();
        nextSealRound = 0;
    }

    public SealEnvelopeResult sealDeferredEnvelope() {
        SealEnvelopeResult result = new SealEnvelopeResult();
        result.status = Status.OK;
        int seal = DigestUtil.FNV_OFFSET;
        List<TapeDigestEntry> digestEntries = new ArrayList<>();
        for (ChannelTapeEntry entry : entries) {
            if (entry.payload == null || entry.payloadLen == 0) continue;
            TapeDigestEntry de = new TapeDigestEntry();
            de.envelopeSeq = entry.envelopeSeq;
            de.channelId = entry.channelId;
            de.payloadLen = entry.payloadLen;
            de.payloadDigest = DigestUtil.fnv1a32(entry.payload, 0, entry.payloadLen);
            digestEntries.add(de);
        }
        digestEntries.sort(Comparator.comparingInt((TapeDigestEntry a) -> a.envelopeSeq)
                .thenComparingInt(a -> a.channelId));
        for (TapeDigestEntry de : digestEntries) {
            seal = DigestUtil.mixDigest(seal, de.payloadDigest);
            seal = DigestUtil.mixDigest(seal, de.channelId);
            seal = DigestUtil.mixDigest(seal, de.envelopeSeq);
            seal = DigestUtil.mixDigest(seal, de.payloadLen);
        }
        for (ChannelTapeEntry entry : entries) {
            if (entry.payload == null || entry.payloadLen == 0) continue;
            entry.sealed = true;
            result.channelsSealed++;
        }
        try {
            for (ChannelTapeEntry entry : entries) {
                if (entry.payload != null && entry.payloadLen > 0) {
                    NativeBridge.nativeStoreHeapBuffer(entry.payload);
                    NativeBridge.nativeQueueChannelEntry(0, entry.payloadLen);
                }
            }
            result.sealDigest = NativeBridge.nativeSealDeferredEnvelope();
        } catch (UnsatisfiedLinkError e) {
            result.sealDigest = seal;
        }
        nextSealRound++;
        return result;
    }

    public List<ChannelTapeEntry> snapshot() {
        return new ArrayList<>(entries);
    }

    public int pendingCount() {
        return entries.size();
    }

    public int unsealedCount() {
        int n = 0;
        for (ChannelTapeEntry e : entries) if (!e.sealed) n++;
        return n;
    }

    public int sealRound() {
        return nextSealRound;
    }

    public long totalPayloadBytes() {
        long sum = 0;
        for (ChannelTapeEntry e : entries) sum += e.payloadLen;
        return sum;
    }

    public List<ChannelTapeEntry> entriesForEnvelope(int envelopeSeq) {
        List<ChannelTapeEntry> result = new ArrayList<>();
        for (ChannelTapeEntry e : entries) {
            if (e.envelopeSeq == envelopeSeq) result.add(e);
        }
        return result;
    }

    public Status verifyAllSealed() {
        for (ChannelTapeEntry e : entries) {
            if (!e.sealed) return Status.BOUNDS_ERROR;
        }
        return Status.OK;
    }

    public int computeEntryDigest(ChannelTapeEntry entry) {
        if (entry.payload == null || entry.payloadLen == 0) return DigestUtil.FNV_OFFSET;
        return DigestUtil.fnv1a32(entry.payload, 0, entry.payloadLen);
    }

    public SealEnvelopeResult resealIfNeeded() {
        if (unsealedCount() == 0) {
            SealEnvelopeResult r = new SealEnvelopeResult();
            r.status = Status.OK;
            return r;
        }
        return sealDeferredEnvelope();
    }

    public void markAllUnsealed() {
        for (ChannelTapeEntry e : entries) e.sealed = false;
    }

    public int maxChannelId() {
        int max = 0;
        for (ChannelTapeEntry e : entries) if (e.channelId > max) max = e.channelId;
        return max;
    }

    public String summarize() {
        return "entries=" + entries.size() + " sealed=" + (entries.size() - unsealedCount())
                + " round=" + nextSealRound;
    }
}
