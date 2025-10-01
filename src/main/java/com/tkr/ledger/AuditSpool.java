package com.tkr.ledger;

import com.tkr.types.WireTypes.AllocationBatchSummary;
import com.tkr.types.WireTypes.Status;
import com.tkr.util.DigestUtil;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Append-only audit spool with chained event digests.
 * Ported from legacy-cpp audit_spool.cc.
 */
public final class AuditSpool {

    public enum AuditEventKind {
        BATCH_PROCESSED, COMPLIANCE_REJECT, MARGIN_BREACH, ENVELOPE_SEAL, SESSION_MERGE, BATCH_INGEST
    }

    public static final class AuditEvent {
        public AuditEventKind kind;
        public int sequence;
        public int deskId;
        public int batchId;
        public int accountId;
        public long timestampNs;
        public String detail = "";
        public int payloadDigest;
    }

    public static final class AuditSpoolConfig {
        public int maxEvents = 8192;
        public boolean computeDigests = true;
    }

    public static final class AuditSpoolResult {
        public Status status = Status.OK;
        public int eventsWritten;
        public int eventsDropped;
        public int chainDigest;
    }

    /** Legacy record type. */
    public static final class AuditRecord {
        public long timestampMillis;
        public AuditEventKind kind;
        public int entityId;
        public int digest;
        public String detail = "";
    }

    private static final AuditSpool GLOBAL = new AuditSpool(new AuditSpoolConfig());

    public static AuditSpool global() {
        return GLOBAL;
    }

    private static final class AuditChainLink {
        int sequence;
        int digest;
        long timestampNs;
    }

    private final AuditSpoolConfig config;
    private final List<AuditEvent> events = new ArrayList<>();
    private int nextSequence = 1;
    private int chainDigest = DigestUtil.FNV_OFFSET;
    private long timestampCounter;

    public AuditSpool() {
        this(new AuditSpoolConfig());
    }

    public AuditSpool(AuditSpoolConfig config) {
        this.config = config != null ? config : new AuditSpoolConfig();
    }

    private long nextTimestamp() {
        return ++timestampCounter;
    }

    private int computeEventDigest(AuditEvent event) {
        byte[] buf = new byte[17];
        buf[0] = (byte) event.kind.ordinal();
        buf[1] = (byte) ((event.sequence >> 24) & 0xFF);
        buf[2] = (byte) ((event.sequence >> 16) & 0xFF);
        buf[3] = (byte) ((event.sequence >> 8) & 0xFF);
        buf[4] = (byte) (event.sequence & 0xFF);
        buf[5] = (byte) ((event.deskId >> 24) & 0xFF);
        buf[6] = (byte) ((event.deskId >> 16) & 0xFF);
        buf[7] = (byte) ((event.deskId >> 8) & 0xFF);
        buf[8] = (byte) (event.deskId & 0xFF);
        buf[9] = (byte) ((event.batchId >> 24) & 0xFF);
        buf[10] = (byte) ((event.batchId >> 16) & 0xFF);
        buf[11] = (byte) ((event.batchId >> 8) & 0xFF);
        buf[12] = (byte) (event.batchId & 0xFF);
        buf[13] = (byte) ((event.accountId >> 24) & 0xFF);
        buf[14] = (byte) ((event.accountId >> 16) & 0xFF);
        buf[15] = (byte) ((event.accountId >> 8) & 0xFF);
        buf[16] = (byte) (event.accountId & 0xFF);
        int digest = DigestUtil.fnv1a32(buf, 0, 17);
        digest = DigestUtil.mixDigest(digest, DigestUtil.fnv1a32(event.detail.getBytes()));
        digest = DigestUtil.mixDigest(digest, event.payloadDigest);
        return digest;
    }

    private void evictOldestIfNeeded() {
        if (events.size() < config.maxEvents) return;
        int evictCount = events.size() - config.maxEvents + 1;
        events.subList(0, evictCount).clear();
    }

    public AuditSpoolResult append(AuditEventKind kind, int deskId, int batchId, int accountId, String detail) {
        AuditSpoolResult result = new AuditSpoolResult();
        evictOldestIfNeeded();
        AuditEvent event = new AuditEvent();
        event.kind = kind;
        event.sequence = nextSequence++;
        event.deskId = deskId;
        event.batchId = batchId;
        event.accountId = accountId;
        event.timestampNs = nextTimestamp();
        event.detail = detail != null ? detail : "";
        event.payloadDigest = 0;
        if (config.computeDigests) {
            event.payloadDigest = computeEventDigest(event);
            chainDigest = DigestUtil.mixDigest(chainDigest, event.payloadDigest);
            chainDigest = DigestUtil.mixDigest(chainDigest, event.sequence);
        }
        events.add(event);
        result.eventsWritten = 1;
        result.chainDigest = chainDigest;
        result.status = Status.OK;
        return result;
    }

    public AuditSpoolResult appendBatchSummary(AllocationBatchSummary summary) {
        String detail = "records=" + summary.recordCount + " qty=" + summary.totalQtyMilli
                + " flags=" + summary.flags + " margin=" + (summary.marginCleared ? "ok" : "fail")
                + " compliance=" + (summary.complianceCleared ? "ok" : "fail");
        AuditEventKind kind = AuditEventKind.BATCH_PROCESSED;
        if (!summary.complianceCleared) kind = AuditEventKind.COMPLIANCE_REJECT;
        else if (!summary.marginCleared) kind = AuditEventKind.MARGIN_BREACH;
        return append(kind, summary.deskId, summary.batchId, 0, detail);
    }

    public AuditSpoolResult flush() {
        AuditSpoolResult result = new AuditSpoolResult();
        result.eventsWritten = events.size();
        result.chainDigest = chainDigest;
        result.status = Status.OK;
        List<AuditChainLink> chain = new ArrayList<>();
        for (AuditEvent event : events) {
            AuditChainLink link = new AuditChainLink();
            link.sequence = event.sequence;
            link.digest = event.payloadDigest;
            link.timestampNs = event.timestampNs;
            chain.add(link);
        }
        chain.sort(Comparator.comparingInt(l -> l.sequence));
        int verify = DigestUtil.FNV_OFFSET;
        for (AuditChainLink link : chain) {
            verify = DigestUtil.mixDigest(verify, link.digest);
            verify = DigestUtil.mixDigest(verify, link.sequence);
        }
        if (verify != chainDigest && !events.isEmpty()) result.eventsDropped = 1;
        return result;
    }

    public void reset() {
        events.clear();
        nextSequence = 1;
        chainDigest = DigestUtil.FNV_OFFSET;
        timestampCounter = 0;
    }

    /** Legacy API. */
    public void append(AuditEventKind kind, int entityId, String detail) {
        append(kind, 0, entityId, 0, detail);
    }

    public List<AuditRecord> snapshot() {
        List<AuditRecord> out = new ArrayList<>();
        for (AuditEvent e : events) {
            AuditRecord r = new AuditRecord();
            r.timestampMillis = e.timestampNs;
            r.kind = e.kind;
            r.entityId = e.batchId;
            r.digest = e.payloadDigest;
            r.detail = e.detail;
            out.add(r);
        }
        return out;
    }

    public int rollingDigest() {
        return chainDigest;
    }

    public int size() {
        return events.size();
    }

    public void clear() {
        reset();
    }

    public Status verifyChain() {
        return flush().eventsDropped == 0 ? Status.OK : Status.BOUNDS_ERROR;
    }

    public List<AuditEvent> eventsSnapshot() {
        return new ArrayList<>(events);
    }

    public AuditEvent lookupSequence(int sequence) {
        for (AuditEvent e : events) {
            if (e.sequence == sequence) return e;
        }
        return null;
    }

    public int countByKind(AuditEventKind kind) {
        int n = 0;
        for (AuditEvent e : events) if (e.kind == kind) n++;
        return n;
    }
}
