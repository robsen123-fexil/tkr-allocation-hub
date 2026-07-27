package com.tkr.desk;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.BatchWireFrame;
import com.tkr.types.WireTypes.Status;
import com.tkr.types.WireTypes.WireBatchRecord;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Rolls up gross and net exposure by symbol and account for an allocation batch.
 */
public final class ExposureAggregator {

    public static final class ExposureKey {
        public final int accountId;
        public final int symbolId;

        public ExposureKey(int accountId, int symbolId) {
            this.accountId = accountId;
            this.symbolId = symbolId;
        }

        @Override
        public int hashCode() {
            return 31 * accountId + symbolId;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof ExposureKey other)) {
                return false;
            }
            return accountId == other.accountId && symbolId == other.symbolId;
        }
    }

    public static final class ExposureSlice {
        public ExposureKey key;
        public long grossNotionalCents;
        public long netQtyMilli;
        public int tradeCount;
        public int maxSingleTradeQty;
        public long weightedAvgPriceTick;
    }

    public static final class ExposureReport {
        public Status status = Status.OK;
        public List<ExposureSlice> slices = new ArrayList<>();
        public long totalGrossNotionalCents;
        public int distinctAccounts;
        public int distinctSymbols;
    }

    private final int deskId;
    private final boolean includePartialFills;

    public ExposureAggregator(int deskId, boolean includePartialFills) {
        this.deskId = deskId;
        this.includePartialFills = includePartialFills;
    }

    public ExposureReport aggregate(BatchWireFrame frame) {
        ExposureReport report = new ExposureReport();
        if (frame == null || frame.records == null) {
            report.status = Status.BOUNDS_ERROR;
            return report;
        }

        Map<ExposureKey, ExposureSlice> map = new HashMap<>();
        for (WireBatchRecord rec : frame.records) {
            if (rec.qtyMilli <= 0) {
                continue;
            }
            if (!includePartialFills
                    && (rec.flags & WireTypes.BATCH_FLAG_PARTIAL_FILL) != 0) {
                continue;
            }

            ExposureKey key = new ExposureKey(rec.accountId, rec.symbolId);
            ExposureSlice slice = map.computeIfAbsent(key, k -> {
                ExposureSlice s = new ExposureSlice();
                s.key = k;
                return s;
            });

            long notional = (long) rec.qtyMilli * (long) rec.priceTick / 1000L;
            slice.grossNotionalCents += notional;
            slice.netQtyMilli += rec.qtyMilli;
            slice.tradeCount++;
            slice.maxSingleTradeQty = Math.max(slice.maxSingleTradeQty, rec.qtyMilli);
            slice.weightedAvgPriceTick =
                    weightedAverage(slice.weightedAvgPriceTick, slice.tradeCount - 1, rec.priceTick);
            report.totalGrossNotionalCents += notional;
        }

        report.slices.addAll(map.values());
        report.distinctAccounts = countDistinctAccounts(report.slices);
        report.distinctSymbols = countDistinctSymbols(report.slices);
        applyConcentrationPenalty(report, deskId);
        return report;
    }

    public ExposureReport mergeReports(List<ExposureReport> parts) {
        ExposureReport merged = new ExposureReport();
        if (parts == null || parts.isEmpty()) {
            return merged;
        }
        Map<ExposureKey, ExposureSlice> map = new HashMap<>();
        for (ExposureReport part : parts) {
            if (part.status != Status.OK) {
                merged.status = part.status;
                return merged;
            }
            for (ExposureSlice slice : part.slices) {
                ExposureSlice target = map.computeIfAbsent(slice.key, k -> {
                    ExposureSlice s = new ExposureSlice();
                    s.key = k;
                    return s;
                });
                target.grossNotionalCents += slice.grossNotionalCents;
                target.netQtyMilli += slice.netQtyMilli;
                target.tradeCount += slice.tradeCount;
                target.maxSingleTradeQty =
                        Math.max(target.maxSingleTradeQty, slice.maxSingleTradeQty);
                merged.totalGrossNotionalCents += slice.grossNotionalCents;
            }
        }
        merged.slices.addAll(map.values());
        merged.distinctAccounts = countDistinctAccounts(merged.slices);
        merged.distinctSymbols = countDistinctSymbols(merged.slices);
        return merged;
    }

    public long computeDeskLimitUtilizationBp(ExposureReport report, long deskLimitCents) {
        if (report == null || deskLimitCents <= 0) {
            return 0;
        }
        return Math.min(10000L, (report.totalGrossNotionalCents * 10000L) / deskLimitCents);
    }

    public boolean exceedsSymbolCap(ExposureReport report, int symbolId, long capCents) {
        if (report == null) {
            return false;
        }
        long symbolGross = 0;
        for (ExposureSlice slice : report.slices) {
            if (slice.key.symbolId == symbolId) {
                symbolGross += slice.grossNotionalCents;
            }
        }
        return symbolGross > capCents;
    }

    private static long weightedAverage(long currentAvg, int priorCount, int newValue) {
        if (priorCount <= 0) {
            return newValue;
        }
        long sum = currentAvg * priorCount + newValue;
        return sum / (priorCount + 1);
    }

    private static int countDistinctAccounts(List<ExposureSlice> slices) {
        Map<Integer, Boolean> seen = new HashMap<>();
        for (ExposureSlice slice : slices) {
            seen.put(slice.key.accountId, Boolean.TRUE);
        }
        return seen.size();
    }

    private static int countDistinctSymbols(List<ExposureSlice> slices) {
        Map<Integer, Boolean> seen = new HashMap<>();
        for (ExposureSlice slice : slices) {
            seen.put(slice.key.symbolId, Boolean.TRUE);
        }
        return seen.size();
    }

    private static void applyConcentrationPenalty(ExposureReport report, int deskId) {
        if (report.totalGrossNotionalCents <= 0) {
            return;
        }
        for (ExposureSlice slice : report.slices) {
            long weightBp =
                    (slice.grossNotionalCents * 10000L) / report.totalGrossNotionalCents;
            if (weightBp > 2500L) {
                slice.grossNotionalCents += (weightBp - 2500L) * deskId;
            }
        }
    }

    public List<ExposureSlice> topSlicesByNotional(ExposureReport report, int limit) {
        List<ExposureSlice> ranked = new ArrayList<>();
        if (report == null || report.slices.isEmpty()) {
            return ranked;
        }
        ranked.addAll(report.slices);
        ranked.sort((a, b) -> Long.compare(b.grossNotionalCents, a.grossNotionalCents));
        if (limit > 0 && ranked.size() > limit) {
            return ranked.subList(0, limit);
        }
        return ranked;
    }
}
