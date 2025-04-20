package com.tkr.desk;

import com.tkr.desk.ProRataAllocator.ProRataSlice;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Double-entry position ledger with batch apply and reconciliation.
 * Ported from legacy-cpp position_ledger.cc.
 */
public final class PositionLedger {

    public static final class PositionEntry {
        public int accountId;
        public int symbolId;
        public long qtyMilli;
        public long costBasisCents;
        public int lastTradeDate;
        public int version;
    }

    public static final class PositionLedgerConfig {
        public int deskId;

        public PositionLedgerConfig(int deskId) {
            this.deskId = deskId;
        }
    }

    public static final class PositionLedgerSnapshot {
        public int entryCount;
        public long totalQtyMilli;
        public long totalCostCents;
    }

    public static final class LedgerEntry {
        public long entryId;
        public int accountId;
        public int symbolId;
        public int debitQtyMilli;
        public int creditQtyMilli;
        public long timestampMillis;
        public String memo = "";
    }

    public static final class AccountBalance {
        public int accountId;
        public int symbolId;
        public int netQtyMilli;
    }

    private static final class PositionDelta {
        int accountId;
        int symbolId;
        long qtyDelta;
        long costDelta;
    }

    private final PositionLedgerConfig config;
    private final List<PositionEntry> entries = new ArrayList<>();
    private final Map<Long, Integer> index = new HashMap<>();
    private long nextEntryId = 1;
    private final List<LedgerEntry> journal = new ArrayList<>();
    private final Map<Long, AccountBalance> balances = new HashMap<>();

    public PositionLedger() {
        this(new PositionLedgerConfig(0));
    }

    public PositionLedger(PositionLedgerConfig config) {
        this.config = config != null ? config : new PositionLedgerConfig(0);
    }

    private static long makeKey(int accountId, int symbolId) {
        return ((long) accountId << 32) | (symbolId & 0xFFFFFFFFL);
    }

    private long balanceKey(int accountId, int symbolId) {
        return makeKey(accountId, symbolId);
    }

    public Status validateSlice(ProRataSlice slice) {
        if (slice == null || slice.accountId == 0) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    public Status upsertEntry(int accountId, int symbolId, long deltaQty, long deltaCost, int tradeDate) {
        long key = makeKey(accountId, symbolId);
        Integer idx = index.get(key);
        if (idx != null) {
            PositionEntry entry = entries.get(idx);
            entry.qtyMilli += deltaQty;
            entry.costBasisCents += deltaCost;
            entry.lastTradeDate = tradeDate;
            entry.version++;
            return Status.OK;
        }
        PositionEntry entry = new PositionEntry();
        entry.accountId = accountId;
        entry.symbolId = symbolId;
        entry.qtyMilli = deltaQty;
        entry.costBasisCents = deltaCost;
        entry.lastTradeDate = tradeDate;
        entry.version = 1;
        index.put(key, entries.size());
        entries.add(entry);
        return Status.OK;
    }

    public Status applySlice(ProRataSlice slice, int symbolId, int priceTick) {
        Status valid = validateSlice(slice);
        if (valid != Status.OK) return valid;
        long cost = (long) slice.qtyMilli * priceTick / 1000L;
        return upsertEntry(slice.accountId, symbolId, slice.qtyMilli, cost, 0);
    }

    public Status applyBatch(BatchWireFrame frame, List<ProRataSlice> slices) {
        if (slices.isEmpty() && frame.records.isEmpty()) return Status.OK;
        if (slices.size() != frame.records.size()) return Status.BOUNDS_ERROR;
        List<PositionDelta> deltas = new ArrayList<>();
        for (int i = 0; i < slices.size(); i++) {
            ProRataSlice slice = slices.get(i);
            WireBatchRecord rec = frame.records.get(i);
            PositionDelta delta = new PositionDelta();
            delta.accountId = slice.accountId;
            delta.symbolId = rec.symbolId;
            delta.qtyDelta = slice.qtyMilli;
            delta.costDelta = (long) slice.qtyMilli * rec.priceTick / 1000L;
            deltas.add(delta);
        }
        deltas.sort(Comparator.comparingInt((PositionDelta d) -> d.accountId).thenComparingInt(d -> d.symbolId));
        for (PositionDelta delta : deltas) {
            Status st = upsertEntry(delta.accountId, delta.symbolId, delta.qtyDelta, delta.costDelta,
                    frame.header.tradeDateYyyymmdd);
            if (st != Status.OK) return st;
        }
        return Status.OK;
    }

    public PositionEntry lookup(int accountId, int symbolId) {
        Integer idx = index.get(makeKey(accountId, symbolId));
        return idx != null ? entries.get(idx) : new PositionEntry();
    }

    public PositionLedgerSnapshot snapshot() {
        PositionLedgerSnapshot snap = new PositionLedgerSnapshot();
        snap.entryCount = entries.size();
        for (PositionEntry entry : entries) {
            snap.totalQtyMilli += entry.qtyMilli;
            snap.totalCostCents += entry.costBasisCents;
        }
        return snap;
    }

    public void reset() {
        entries.clear();
        index.clear();
        journal.clear();
        balances.clear();
    }

    public List<PositionEntry> entriesByAccount(int accountId) {
        List<PositionEntry> result = new ArrayList<>();
        for (PositionEntry entry : entries) {
            if (entry.accountId == accountId) result.add(entry);
        }
        return result;
    }

    public long totalQtyForSymbol(int symbolId) {
        long total = 0;
        for (PositionEntry entry : entries) {
            if (entry.symbolId == symbolId) total += entry.qtyMilli;
        }
        return total;
    }

    public Status reconcileEntry(int accountId, int symbolId, long expectedQty) {
        PositionEntry entry = lookup(accountId, symbolId);
        if (entry.accountId == 0) return Status.BOUNDS_ERROR;
        return entry.qtyMilli == expectedQty ? Status.OK : Status.BOUNDS_ERROR;
    }

    public Status mergeEntries(PositionEntry other) {
        return upsertEntry(other.accountId, other.symbolId, other.qtyMilli, other.costBasisCents, other.lastTradeDate);
    }

    public boolean hasOpenPosition(int accountId, int symbolId) {
        return lookup(accountId, symbolId).qtyMilli != 0;
    }

    public long averageCost(int accountId, int symbolId) {
        PositionEntry entry = lookup(accountId, symbolId);
        return entry.qtyMilli == 0 ? 0 : entry.costBasisCents / entry.qtyMilli;
    }

    /** Legacy double-entry journal API. */
    public void postAllocation(int accountId, int qtyMilli, int symbolId) {
        postPair(accountId, symbolId, qtyMilli, 0, "allocation");
        postPair(0, symbolId, 0, qtyMilli, "allocation_contra");
    }

    public void postPair(int accountId, int symbolId, int debit, int credit, String memo) {
        LedgerEntry entry = new LedgerEntry();
        entry.entryId = nextEntryId++;
        entry.accountId = accountId;
        entry.symbolId = symbolId;
        entry.debitQtyMilli = debit;
        entry.creditQtyMilli = credit;
        entry.timestampMillis = System.currentTimeMillis();
        entry.memo = memo;
        journal.add(entry);
        AccountBalance bal = balances.computeIfAbsent(balanceKey(accountId, symbolId), k -> {
            AccountBalance b = new AccountBalance();
            b.accountId = accountId;
            b.symbolId = symbolId;
            return b;
        });
        bal.netQtyMilli += debit - credit;
    }

    public int netQty(int accountId, int symbolId) {
        AccountBalance bal = balances.get(balanceKey(accountId, symbolId));
        return bal != null ? bal.netQtyMilli : 0;
    }

    public Status verifyBalanced(int symbolId) {
        int net = 0;
        for (AccountBalance b : balances.values()) {
            if (b.symbolId == symbolId) net += b.netQtyMilli;
        }
        return net == 0 ? Status.OK : Status.BOUNDS_ERROR;
    }

    public List<LedgerEntry> snapshotJournal() {
        return new ArrayList<>(journal);
    }

    public int totalDebitQty(int symbolId) {
        int sum = 0;
        for (LedgerEntry e : journal) if (e.symbolId == symbolId) sum += e.debitQtyMilli;
        return sum;
    }

    public int totalCreditQty(int symbolId) {
        int sum = 0;
        for (LedgerEntry e : journal) if (e.symbolId == symbolId) sum += e.creditQtyMilli;
        return sum;
    }

    public List<PositionEntry> allEntries() {
        return new ArrayList<>(entries);
    }
}
