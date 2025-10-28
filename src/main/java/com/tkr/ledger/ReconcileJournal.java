package com.tkr.ledger;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Double-entry reconcile journal with external matching and write-off.
 * Ported from legacy-cpp reconcile_journal.cc.
 */
public final class ReconcileJournal {

    public enum JournalEntryKind {
        ALLOCATION_DEBIT, ALLOCATION_CREDIT, MARGIN_RESERVE, FEE_ACCRUAL, RECONCILE_ADJUSTMENT
    }

    public enum ReconcileState { PENDING, MATCHED, ADJUSTED, UNMATCHED, WRITTEN_OFF }

    public static final class JournalLine {
        public int lineId;
        public JournalEntryKind kind;
        public int batchId;
        public int accountId;
        public int symbolId;
        public long qtyMilli;
        public long amountCents;
        public int tradeDateYyyymmdd;
        public int settleDateYyyymmdd;
        public ReconcileState state = ReconcileState.PENDING;
        public String reference = "";
    }

    public static final class ReconcileMatch {
        public int internalLineId;
        public int externalLineId;
        public long qtyDelta;
        public long amountDelta;
        public ReconcileState resultingState;
    }

    public static final class ReconcileJournalConfig {
        public int maxEntries = 8192;
        public boolean autoMatchExact = true;
        public long qtyToleranceMilli;
        public long amountToleranceCents;
    }

    public static final class ReconcileJournalResult {
        public Status status = Status.OK;
        public List<JournalLine> entries = new ArrayList<>();
        public List<ReconcileMatch> matches = new ArrayList<>();
        public int matchedCount;
        public int unmatchedCount;
        public long netQtyMilli;
        public long netAmountCents;
    }

    private static final ReconcileJournal GLOBAL = new ReconcileJournal(new ReconcileJournalConfig());

    public static ReconcileJournal global() {
        return GLOBAL;
    }

    private final ReconcileJournalConfig config;
    private final List<JournalLine> lines = new ArrayList<>();
    private int nextLineId = 1;

    public ReconcileJournal() {
        this(new ReconcileJournalConfig());
    }

    public ReconcileJournal(ReconcileJournalConfig config) {
        this.config = config != null ? config : new ReconcileJournalConfig();
    }

    private int nextLineId() {
        return nextLineId++;
    }

    private void evictOldestIfNeeded() {
        while (lines.size() >= config.maxEntries) lines.remove(0);
    }

    public JournalLine makeAllocationDebit(int batchId, int accountId, int symbolId, long qtyMilli, long amountCents) {
        JournalLine line = new JournalLine();
        line.kind = JournalEntryKind.ALLOCATION_DEBIT;
        line.batchId = batchId;
        line.accountId = accountId;
        line.symbolId = symbolId;
        line.qtyMilli = qtyMilli;
        line.amountCents = amountCents;
        line.state = ReconcileState.PENDING;
        return line;
    }

    public JournalLine makeAllocationCredit(int batchId, int accountId, int symbolId, long qtyMilli, long amountCents) {
        JournalLine line = makeAllocationDebit(batchId, accountId, symbolId, qtyMilli, amountCents);
        line.kind = JournalEntryKind.ALLOCATION_CREDIT;
        line.qtyMilli = -qtyMilli;
        line.amountCents = -amountCents;
        return line;
    }

    public Status postLine(JournalLine line) {
        evictOldestIfNeeded();
        JournalLine posted = line;
        posted.lineId = nextLineId();
        lines.add(posted);
        return Status.OK;
    }

    public Status postBatch(BatchWireFrame frame, int settleDateYyyymmdd) {
        for (WireBatchRecord rec : frame.records) {
            long amount = (long) rec.qtyMilli * rec.priceTick / 1000L;
            JournalLine debit = makeAllocationDebit(frame.header.deskId, rec.accountId, rec.symbolId, rec.qtyMilli, amount);
            debit.tradeDateYyyymmdd = frame.header.tradeDateYyyymmdd;
            debit.settleDateYyyymmdd = settleDateYyyymmdd;
            debit.reference = "batch:" + rec.recordId;
            Status st = postLine(debit);
            if (st != Status.OK) return st;
            JournalLine credit = makeAllocationCredit(frame.header.deskId, rec.accountId, rec.symbolId, rec.qtyMilli, amount);
            credit.tradeDateYyyymmdd = frame.header.tradeDateYyyymmdd;
            credit.settleDateYyyymmdd = settleDateYyyymmdd;
            credit.reference = "batch:" + rec.recordId;
            st = postLine(credit);
            if (st != Status.OK) return st;
            if ((frame.header.flags & WireTypes.BATCH_FLAG_MARGIN_CHECK) != 0) {
                JournalLine margin = new JournalLine();
                margin.kind = JournalEntryKind.MARGIN_RESERVE;
                margin.batchId = frame.header.deskId;
                margin.accountId = rec.accountId;
                margin.symbolId = rec.symbolId;
                margin.amountCents = amount / 10;
                margin.tradeDateYyyymmdd = frame.header.tradeDateYyyymmdd;
                margin.settleDateYyyymmdd = settleDateYyyymmdd;
                margin.reference = "margin:" + rec.recordId;
                st = postLine(margin);
                if (st != Status.OK) return st;
            }
        }
        return Status.OK;
    }

    public JournalLine lookup(int lineId) {
        for (JournalLine line : lines) {
            if (line.lineId == lineId) return line;
        }
        return new JournalLine();
    }

    public boolean linesMatch(JournalLine internal, JournalLine external) {
        if (internal.accountId != external.accountId || internal.symbolId != external.symbolId) return false;
        long qtyDiff = internal.qtyMilli - external.qtyMilli;
        if (Math.abs(qtyDiff) > config.qtyToleranceMilli) return false;
        long amtDiff = internal.amountCents - external.amountCents;
        return Math.abs(amtDiff) <= config.amountToleranceCents;
    }

    public ReconcileMatch attemptMatch(JournalLine internal, JournalLine external) {
        ReconcileMatch match = new ReconcileMatch();
        match.internalLineId = internal.lineId;
        match.externalLineId = external.lineId;
        match.qtyDelta = internal.qtyMilli - external.qtyMilli;
        match.amountDelta = internal.amountCents - external.amountCents;
        if (match.qtyDelta == 0 && match.amountDelta == 0) {
            match.resultingState = ReconcileState.MATCHED;
            internal.state = ReconcileState.MATCHED;
            external.state = ReconcileState.MATCHED;
        } else if (Math.abs(match.qtyDelta) <= config.qtyToleranceMilli
                && Math.abs(match.amountDelta) <= config.amountToleranceCents) {
            match.resultingState = ReconcileState.ADJUSTED;
            internal.state = ReconcileState.ADJUSTED;
            external.state = ReconcileState.ADJUSTED;
        } else {
            match.resultingState = ReconcileState.UNMATCHED;
        }
        return match;
    }

    public ReconcileJournalResult reconcile() {
        ReconcileJournalResult result = new ReconcileJournalResult();
        result.status = Status.OK;
        result.entries = new ArrayList<>(lines);
        for (JournalLine line : lines) {
            result.netQtyMilli += line.qtyMilli;
            result.netAmountCents += line.amountCents;
            if (line.state == ReconcileState.PENDING) result.unmatchedCount++;
            else if (line.state == ReconcileState.MATCHED || line.state == ReconcileState.ADJUSTED) result.matchedCount++;
        }
        if (config.autoMatchExact && result.netQtyMilli == 0 && result.netAmountCents == 0) {
            for (JournalLine line : lines) {
                if (line.state == ReconcileState.PENDING) {
                    line.state = ReconcileState.MATCHED;
                    result.matchedCount++;
                    result.unmatchedCount--;
                }
            }
        }
        return result;
    }

    public ReconcileJournalResult matchExternal(List<JournalLine> externalLines) {
        ReconcileJournalResult result = new ReconcileJournalResult();
        result.status = Status.OK;
        List<JournalLine> externals = new ArrayList<>(externalLines);
        for (JournalLine internal : lines) {
            if (internal.state != ReconcileState.PENDING) continue;
            for (JournalLine external : externals) {
                if (external.state != ReconcileState.PENDING) continue;
                if (!linesMatch(internal, external)) continue;
                ReconcileMatch match = attemptMatch(internal, external);
                result.matches.add(match);
                if (match.resultingState == ReconcileState.MATCHED || match.resultingState == ReconcileState.ADJUSTED) {
                    result.matchedCount++;
                } else {
                    result.unmatchedCount++;
                }
                break;
            }
        }
        for (JournalLine line : lines) {
            result.netQtyMilli += line.qtyMilli;
            result.netAmountCents += line.amountCents;
        }
        result.entries = new ArrayList<>(lines);
        return result;
    }

    public ReconcileJournalResult summarizeByAccount() {
        ReconcileJournalResult result = new ReconcileJournalResult();
        result.status = Status.OK;
        Map<Integer, JournalLine> byAccount = new HashMap<>();
        for (JournalLine line : lines) {
            JournalLine agg = byAccount.computeIfAbsent(line.accountId, id -> {
                JournalLine j = new JournalLine();
                j.accountId = id;
                return j;
            });
            agg.qtyMilli += line.qtyMilli;
            agg.amountCents += line.amountCents;
        }
        for (JournalLine agg : byAccount.values()) {
            result.entries.add(agg);
            result.netQtyMilli += agg.qtyMilli;
            result.netAmountCents += agg.amountCents;
        }
        return result;
    }

    public Status writeOffUnmatched() {
        for (JournalLine line : lines) {
            if (line.state == ReconcileState.UNMATCHED || line.state == ReconcileState.PENDING) {
                line.state = ReconcileState.WRITTEN_OFF;
                line.kind = JournalEntryKind.RECONCILE_ADJUSTMENT;
            }
        }
        return Status.OK;
    }

    public List<JournalLine> pendingLines() {
        List<JournalLine> pending = new ArrayList<>();
        for (JournalLine line : lines) {
            if (line.state == ReconcileState.PENDING) pending.add(line);
        }
        return pending;
    }

    public ReconcileJournalResult verifyBalanced() {
        ReconcileJournalResult result = reconcile();
        if (result.netQtyMilli != 0 || result.netAmountCents != 0) result.status = Status.BOUNDS_ERROR;
        return result;
    }

    public ReconcileJournalResult filterByBatch(int batchId) {
        ReconcileJournalResult result = new ReconcileJournalResult();
        result.status = Status.OK;
        for (JournalLine line : lines) {
            if (line.batchId != batchId) continue;
            result.entries.add(line);
            result.netQtyMilli += line.qtyMilli;
            result.netAmountCents += line.amountCents;
            if (line.state == ReconcileState.MATCHED || line.state == ReconcileState.ADJUSTED) result.matchedCount++;
            else result.unmatchedCount++;
        }
        return result;
    }

    public Status markMatched(int lineId) {
        for (JournalLine line : lines) {
            if (line.lineId == lineId) {
                line.state = ReconcileState.MATCHED;
                return Status.OK;
            }
        }
        return Status.BOUNDS_ERROR;
    }

    public void reset() {
        lines.clear();
        nextLineId = 1;
    }

    public int lineCount() {
        return lines.size();
    }

    public long totalDebitsForBatch(int batchId) {
        long sum = 0;
        for (JournalLine line : lines) {
            if (line.batchId == batchId && line.qtyMilli > 0) sum += line.qtyMilli;
        }
        return sum;
    }

    public long totalCreditsForBatch(int batchId) {
        long sum = 0;
        for (JournalLine line : lines) {
            if (line.batchId == batchId && line.qtyMilli < 0) sum += -line.qtyMilli;
        }
        return sum;
    }

    public List<JournalLine> linesByAccount(int accountId) {
        List<JournalLine> result = new ArrayList<>();
        for (JournalLine line : lines) {
            if (line.accountId == accountId) result.add(line);
        }
        return result;
    }

    public ReconcileJournalResult reconcileBatch(int batchId) {
        ReconcileJournalResult filtered = filterByBatch(batchId);
        filtered.status = filtered.netQtyMilli == 0 && filtered.netAmountCents == 0 ? Status.OK : Status.BOUNDS_ERROR;
        return filtered;
    }

    public String formatLine(JournalLine line) {
        return "id=" + line.lineId + " kind=" + line.kind + " acct=" + line.accountId
                + " qty=" + line.qtyMilli + " amt=" + line.amountCents + " state=" + line.state;
    }
}
