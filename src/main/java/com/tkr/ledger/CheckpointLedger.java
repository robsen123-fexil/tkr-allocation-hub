package com.tkr.ledger;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;
import com.tkr.util.DigestUtil;

import java.util.ArrayList;
import java.util.List;

/**
 * Checkpoint ledger for session merge digest sealing and chain verification.
 * Ported from legacy-cpp checkpoint_ledger.cc.
 */
public final class CheckpointLedger {

    public static final class CheckpointRecord {
        public int checkpointSeq;
        public int sessionId;
        public int legCount;
        public int mergeDigest;
        public int prevCheckpointSeq;
        public long sealedAtNs;
        public boolean valid;
    }

    public static final class CheckpointLedgerConfig {
        public int maxCheckpoints = 1024;
        public boolean verifyChain = true;
    }

    public static final class CheckpointSealResult {
        public Status status = Status.OK;
        public int checkpointSeq;
        public int mergeDigest;
        public boolean chainValid;
    }

    public static final class CheckpointVerifyResult {
        public Status status = Status.OK;
        public int checkpointsVerified;
        public boolean chainIntact;
        public int firstGapSeq;
    }

    private static final CheckpointLedger GLOBAL = new CheckpointLedger(new CheckpointLedgerConfig());

    public static CheckpointLedger global() {
        return GLOBAL;
    }

    private final CheckpointLedgerConfig config;
    private final List<CheckpointRecord> records = new ArrayList<>();
    private int latestSeq;

    public CheckpointLedger() {
        this(new CheckpointLedgerConfig());
    }

    public CheckpointLedger(CheckpointLedgerConfig config) {
        this.config = config != null ? config : new CheckpointLedgerConfig();
    }

    public int computeMergeDigest(SessionWireFrame frame) {
        int digest = DigestUtil.FNV_OFFSET;
        digest = DigestUtil.mixDigest(digest, frame.header.sessionId);
        digest = DigestUtil.mixDigest(digest, frame.header.legCount);
        digest = DigestUtil.mixDigest(digest, frame.header.checkpointSeq);
        digest = DigestUtil.mixDigest(digest, frame.header.flags);
        for (WireSessionLeg leg : frame.legs) {
            digest = DigestUtil.mixDigest(digest, leg.legId);
            digest = DigestUtil.mixDigest(digest, leg.clOrdId);
            digest = DigestUtil.mixDigest(digest, leg.allocAccount);
            digest = DigestUtil.mixDigest(digest, leg.qtyMilli);
            digest = DigestUtil.mixDigest(digest, leg.flags);
            if (leg.refLen > 0 && leg.refOffset < frame.refBlob.length) {
                int avail = frame.refBlob.length - leg.refOffset;
                int len = Math.min(leg.refLen, avail);
                digest = DigestUtil.mixDigest(digest, DigestUtil.fnv1a32(frame.refBlob, leg.refOffset, len));
            }
        }
        for (MergeSlot slot : frame.mergeSlots) {
            if (!slot.pinned) continue;
            digest = DigestUtil.mixDigest(digest, slot.slotId);
            digest = DigestUtil.mixDigest(digest, slot.legId);
            digest = DigestUtil.mixDigest(digest, slot.checkpointSeq);
            if (slot.refData != null && slot.refLen > 0) {
                digest = DigestUtil.mixDigest(digest, DigestUtil.fnv1a32(slot.refData, 0, slot.refLen));
            }
        }
        return digest;
    }

    private void evictOldestIfNeeded() {
        if (records.size() >= config.maxCheckpoints) records.remove(0);
    }

    private boolean validateSequenceChain() {
        if (records.isEmpty()) return true;
        int expectedPrev = 0;
        for (CheckpointRecord rec : records) {
            if (rec.checkpointSeq != expectedPrev + 1) return false;
            if (rec.prevCheckpointSeq != expectedPrev) return false;
            if (!rec.valid) return false;
            expectedPrev = rec.checkpointSeq;
        }
        return true;
    }

    public CheckpointSealResult sealSession(SessionWireFrame frame) {
        CheckpointSealResult result = new CheckpointSealResult();
        evictOldestIfNeeded();
        CheckpointRecord record = new CheckpointRecord();
        record.checkpointSeq = latestSeq + 1;
        record.sessionId = frame.header.sessionId;
        record.legCount = frame.header.legCount;
        record.mergeDigest = computeMergeDigest(frame);
        record.prevCheckpointSeq = latestSeq;
        record.sealedAtNs = (long) record.checkpointSeq * 1_000_000L;
        record.valid = true;
        if (config.verifyChain && latestSeq > 0) {
            CheckpointRecord prev = lookup(latestSeq);
            if (prev.sessionId != 0 && prev.sessionId != record.sessionId) record.valid = false;
        }
        records.add(record);
        latestSeq = record.checkpointSeq;
        result.checkpointSeq = record.checkpointSeq;
        result.mergeDigest = record.mergeDigest;
        result.chainValid = validateSequenceChain();
        result.status = result.chainValid ? Status.OK : Status.SESSION_GAP;
        return result;
    }

    public CheckpointVerifyResult verifyChain() {
        CheckpointVerifyResult result = new CheckpointVerifyResult();
        result.checkpointsVerified = records.size();
        result.chainIntact = validateSequenceChain();
        result.status = result.chainIntact ? Status.OK : Status.SESSION_GAP;
        if (!result.chainIntact) {
            int expected = 0;
            for (CheckpointRecord rec : records) {
                if (rec.checkpointSeq != expected + 1) {
                    result.firstGapSeq = expected + 1;
                    break;
                }
                expected = rec.checkpointSeq;
            }
        }
        return result;
    }

    public CheckpointRecord lookup(int checkpointSeq) {
        for (CheckpointRecord rec : records) {
            if (rec.checkpointSeq == checkpointSeq) return rec;
        }
        return new CheckpointRecord();
    }

    public void reset() {
        records.clear();
        latestSeq = 0;
    }

    public int latestCheckpointSeq() {
        return latestSeq;
    }

    public List<CheckpointRecord> snapshot() {
        return new ArrayList<>(records);
    }

    public boolean hasGapAfter(int seq) {
        for (CheckpointRecord rec : records) {
            if (rec.prevCheckpointSeq == seq && rec.checkpointSeq != seq + 1) return true;
        }
        return false;
    }

    public int digestAt(int checkpointSeq) {
        CheckpointRecord rec = lookup(checkpointSeq);
        return rec.mergeDigest;
    }

    public Status pinCheckpoint(int checkpointSeq, SessionWireFrame frame) {
        CheckpointRecord rec = lookup(checkpointSeq);
        if (rec.checkpointSeq == 0) return Status.BOUNDS_ERROR;
        int computed = computeMergeDigest(frame);
        return computed == rec.mergeDigest ? Status.OK : Status.BOUNDS_ERROR;
    }

    public String summarizeChain() {
        return "checkpoints=" + records.size() + " latest=" + latestSeq + " valid=" + validateSequenceChain();
    }
}
