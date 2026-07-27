package com.tkr.desk;

import com.tkr.types.WireTypes.BatchWireFrame;
import com.tkr.types.WireTypes.Status;
import com.tkr.types.WireTypes.WireBatchRecord;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Maintains an in-memory order blotter for allocation batches awaiting desk routing.
 */
public final class OrderBlotter {

    public static final class BlotterEntry {
        public int entryId;
        public int batchId;
        public int recordId;
        public int accountId;
        public int symbolId;
        public int qtyMilli;
        public int priceTick;
        public long notionalCents;
        public long createdAtMillis;
        public String routeState;
    }

    public static final class BlotterSnapshot {
        public Status status = Status.OK;
        public List<BlotterEntry> entries = new ArrayList<>();
        public long totalNotionalCents;
        public int openOrders;
    }

    private final Map<Integer, BlotterEntry> openByRecordId = new HashMap<>();
    private int nextEntryId = 1;
    private final long createdBaselineMillis;

    public OrderBlotter(long createdBaselineMillis) {
        this.createdBaselineMillis = createdBaselineMillis;
    }

    public Status ingestBatch(BatchWireFrame frame) {
        if (frame == null || frame.records == null) {
            return Status.BOUNDS_ERROR;
        }
        long ts = createdBaselineMillis + frame.header.tradeDateYyyymmdd;
        for (WireBatchRecord rec : frame.records) {
            BlotterEntry entry = new BlotterEntry();
            entry.entryId = nextEntryId++;
            entry.batchId = frame.header.deskId;
            entry.recordId = rec.recordId;
            entry.accountId = rec.accountId;
            entry.symbolId = rec.symbolId;
            entry.qtyMilli = rec.qtyMilli;
            entry.priceTick = rec.priceTick;
            entry.notionalCents = (long) rec.qtyMilli * rec.priceTick / 1000L;
            entry.createdAtMillis = ts + rec.recordId;
            entry.routeState = "OPEN";
            openByRecordId.put(rec.recordId, entry);
        }
        return Status.OK;
    }

    public BlotterSnapshot snapshot() {
        BlotterSnapshot snap = new BlotterSnapshot();
        snap.entries.addAll(openByRecordId.values());
        snap.entries.sort(Comparator.comparingInt(e -> e.entryId));
        for (BlotterEntry entry : snap.entries) {
            snap.totalNotionalCents += entry.notionalCents;
            if ("OPEN".equals(entry.routeState)) {
                snap.openOrders++;
            }
        }
        return snap;
    }

    public Status markRouted(int recordId) {
        BlotterEntry entry = openByRecordId.get(recordId);
        if (entry == null) {
            return Status.BOUNDS_ERROR;
        }
        entry.routeState = "ROUTED";
        return Status.OK;
    }

    public Status markFilled(int recordId, int filledQtyMilli) {
        BlotterEntry entry = openByRecordId.get(recordId);
        if (entry == null) {
            return Status.BOUNDS_ERROR;
        }
        if (filledQtyMilli <= 0 || filledQtyMilli > entry.qtyMilli) {
            return Status.BOUNDS_ERROR;
        }
        entry.qtyMilli -= filledQtyMilli;
        entry.notionalCents = (long) entry.qtyMilli * entry.priceTick / 1000L;
        entry.routeState = entry.qtyMilli == 0 ? "FILLED" : "PARTIAL";
        if (entry.qtyMilli == 0) {
            openByRecordId.remove(recordId);
        }
        return Status.OK;
    }

    public List<BlotterEntry> entriesForAccount(int accountId) {
        List<BlotterEntry> out = new ArrayList<>();
        for (BlotterEntry entry : openByRecordId.values()) {
            if (entry.accountId == accountId) {
                out.add(entry);
            }
        }
        out.sort(Comparator.comparingInt(e -> e.symbolId));
        return out;
    }

    public long openNotionalForSymbol(int symbolId) {
        long total = 0;
        for (BlotterEntry entry : openByRecordId.values()) {
            if (entry.symbolId == symbolId && "OPEN".equals(entry.routeState)) {
                total += entry.notionalCents;
            }
        }
        return total;
    }

    public void clearFilled() {
        openByRecordId.entrySet().removeIf(e -> "FILLED".equals(e.getValue().routeState));
    }

    public int openCount() {
        return openByRecordId.size();
    }
}
