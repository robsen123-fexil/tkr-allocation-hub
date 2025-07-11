package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Tax lot matcher: FIFO, LIFO, HIFO, specific lot, average cost.
 * Ported from legacy-cpp tax_lot_matcher.cc.
 */
public final class TaxLotMatcher {

    public enum TaxLotMatchMethod { FIFO, LIFO, HIFO, SPECIFIC_LOT, AVERAGE_COST }

    public enum MatchMethod { FIFO, LIFO, HIFO }

    public static final class TaxLot {
        public int lotId;
        public int accountId;
        public int symbolId;
        public int qtyMilli;
        public long costBasisCents;
        public int acquireDateYyyymmdd;
        public long acquiredMillis;
        public boolean closed;
    }

    public static final class TaxLotMatchRequest {
        public int accountId;
        public int symbolId;
        public int sellQtyMilli;
        public TaxLotMatchMethod method = TaxLotMatchMethod.FIFO;
        public int specificLotId;
    }

    public static final class TaxLotMatchPair {
        public int lotId;
        public int matchedQtyMilli;
        public long costBasisCents;
        public long gainLossCents;
    }

    public static final class TaxLotMatcherConfig {
        public TaxLotMatchMethod defaultMethod = TaxLotMatchMethod.FIFO;
    }

    public static final class TaxLotMatcherResult {
        public Status status = Status.OK;
        public List<TaxLotMatchPair> matches = new ArrayList<>();
        public int totalMatchedQty;
        public long totalGainLossCents;
        public int remainingQty;
    }

    public static final class MatchRequest {
        public int accountId;
        public int symbolId;
        public int sellQtyMilli;
        public MatchMethod method = MatchMethod.FIFO;
    }

    public static final class MatchResult {
        public Status status = Status.OK;
        public List<TaxLot> consumedLots = new ArrayList<>();
        public long realizedGainCents;
        public int unmatchedQtyMilli;
    }

    private final TaxLotMatcherConfig config;
    private final List<TaxLot> openLots = new ArrayList<>();
    private int nextLotId = 1;

    public TaxLotMatcher() {
        this(new TaxLotMatcherConfig());
    }

    public TaxLotMatcher(TaxLotMatcherConfig config) {
        this.config = config != null ? config : new TaxLotMatcherConfig();
    }

    public void openLot(TaxLot lot) {
        if (lot != null) openLots.add(lot);
    }

    public TaxLot openLot(int accountId, int symbolId, int qtyMilli, long costBasisCents) {
        TaxLot lot = new TaxLot();
        lot.lotId = nextLotId++;
        lot.accountId = accountId;
        lot.symbolId = symbolId;
        lot.qtyMilli = qtyMilli;
        lot.costBasisCents = costBasisCents;
        lot.acquireDateYyyymmdd = 0;
        lot.acquiredMillis = System.currentTimeMillis();
        lot.closed = false;
        openLots.add(lot);
        return lot;
    }

    public void clearLots() {
        openLots.clear();
    }

    public TaxLot findLot(int lotId) {
        for (TaxLot lot : openLots) {
            if (lot.lotId == lotId && !lot.closed) return lot;
        }
        return null;
    }

    public void sortLotsByAcquireDate(boolean ascending) {
        openLots.sort(ascending
                ? Comparator.comparingInt(l -> l.acquireDateYyyymmdd)
                : Comparator.comparingInt((TaxLot l) -> l.acquireDateYyyymmdd).reversed());
    }

    public void sortLotsByCostBasis(boolean highestFirst) {
        openLots.sort(highestFirst
                ? Comparator.comparingLong((TaxLot l) -> l.costBasisCents).reversed()
                : Comparator.comparingLong(l -> l.costBasisCents));
    }

    public static long computeGainLoss(long proceedsCents, long costCents) {
        return proceedsCents - costCents;
    }

    public TaxLotMatcherResult matchFifo(TaxLotMatchRequest request) {
        TaxLotMatcherResult result = new TaxLotMatcherResult();
        result.status = Status.OK;
        sortLotsByAcquireDate(true);
        int remaining = request.sellQtyMilli;
        for (TaxLot lot : openLots) {
            if (lot.closed || lot.accountId != request.accountId || lot.symbolId != request.symbolId) continue;
            if (remaining <= 0) break;
            int matched = Math.min(lot.qtyMilli, remaining);
            long costPerUnit = lot.qtyMilli > 0 ? lot.costBasisCents / lot.qtyMilli : 0;
            long matchedCost = costPerUnit * matched;
            TaxLotMatchPair pair = new TaxLotMatchPair();
            pair.lotId = lot.lotId;
            pair.matchedQtyMilli = matched;
            pair.costBasisCents = matchedCost;
            pair.gainLossCents = computeGainLoss(0, matchedCost);
            result.matches.add(pair);
            lot.qtyMilli -= matched;
            if (lot.qtyMilli == 0) lot.closed = true;
            remaining -= matched;
            result.totalMatchedQty += matched;
            result.totalGainLossCents += pair.gainLossCents;
        }
        result.remainingQty = remaining;
        return result;
    }

    public TaxLotMatcherResult matchLifo(TaxLotMatchRequest request) {
        sortLotsByAcquireDate(false);
        return matchFifo(request);
    }

    public TaxLotMatcherResult matchHifo(TaxLotMatchRequest request) {
        sortLotsByCostBasis(true);
        return matchFifo(request);
    }

    public TaxLotMatcherResult matchSpecific(TaxLotMatchRequest request) {
        TaxLotMatcherResult result = new TaxLotMatcherResult();
        TaxLot lot = findLot(request.specificLotId);
        if (lot == null) {
            result.status = Status.BOUNDS_ERROR;
            return result;
        }
        int matched = Math.min(lot.qtyMilli, request.sellQtyMilli);
        TaxLotMatchPair pair = new TaxLotMatchPair();
        pair.lotId = lot.lotId;
        pair.matchedQtyMilli = matched;
        pair.costBasisCents = lot.qtyMilli > 0 ? lot.costBasisCents * matched / lot.qtyMilli : 0;
        pair.gainLossCents = computeGainLoss(0, pair.costBasisCents);
        result.matches.add(pair);
        lot.qtyMilli -= matched;
        if (lot.qtyMilli == 0) lot.closed = true;
        result.totalMatchedQty = matched;
        result.totalGainLossCents = pair.gainLossCents;
        result.remainingQty = request.sellQtyMilli - matched;
        result.status = Status.OK;
        return result;
    }

    public TaxLotMatcherResult matchAverageCost(TaxLotMatchRequest request) {
        TaxLotMatcherResult result = new TaxLotMatcherResult();
        result.status = Status.OK;
        int totalQty = 0;
        long totalCost = 0;
        for (TaxLot lot : openLots) {
            if (lot.closed || lot.accountId != request.accountId || lot.symbolId != request.symbolId) continue;
            totalQty += lot.qtyMilli;
            totalCost += lot.costBasisCents;
        }
        if (totalQty == 0) {
            result.status = Status.BOUNDS_ERROR;
            return result;
        }
        int matched = request.sellQtyMilli;
        long avgCost = totalCost * matched / totalQty;
        TaxLotMatchPair pair = new TaxLotMatchPair();
        pair.lotId = 0;
        pair.matchedQtyMilli = matched;
        pair.costBasisCents = avgCost;
        pair.gainLossCents = computeGainLoss(0, avgCost);
        result.matches.add(pair);
        result.totalMatchedQty = matched;
        result.totalGainLossCents = avgCost;
        int toReduce = matched;
        for (TaxLot lot : openLots) {
            if (lot.closed || lot.accountId != request.accountId || lot.symbolId != request.symbolId) continue;
            int reduce = Math.min(lot.qtyMilli, toReduce);
            lot.qtyMilli -= reduce;
            lot.costBasisCents -= avgCost * reduce / matched;
            if (lot.qtyMilli == 0) lot.closed = true;
            toReduce -= reduce;
            if (toReduce == 0) break;
        }
        return result;
    }

    public TaxLotMatcherResult match(TaxLotMatchRequest request) {
        return switch (request.method) {
            case LIFO -> matchLifo(request);
            case HIFO -> matchHifo(request);
            case SPECIFIC_LOT -> matchSpecific(request);
            case AVERAGE_COST -> matchAverageCost(request);
            default -> matchFifo(request);
        };
    }

    public TaxLotMatcherResult matchBatch(BatchWireFrame frame) {
        TaxLotMatcherResult combined = new TaxLotMatcherResult();
        combined.status = Status.OK;
        for (WireBatchRecord rec : frame.records) {
            TaxLotMatchRequest req = new TaxLotMatchRequest();
            req.accountId = rec.accountId;
            req.symbolId = rec.symbolId;
            req.sellQtyMilli = rec.qtyMilli;
            req.method = config.defaultMethod;
            TaxLotMatcherResult partial = match(req);
            if (partial.status != Status.OK) {
                combined.status = partial.status;
                return combined;
            }
            combined.matches.addAll(partial.matches);
            combined.totalMatchedQty += partial.totalMatchedQty;
            combined.totalGainLossCents += partial.totalGainLossCents;
        }
        return combined;
    }

    /** Legacy API. */
    public MatchResult match(MatchRequest request) {
        TaxLotMatchRequest req = new TaxLotMatchRequest();
        req.accountId = request.accountId;
        req.symbolId = request.symbolId;
        req.sellQtyMilli = request.sellQtyMilli;
        req.method = switch (request.method) {
            case LIFO -> TaxLotMatchMethod.LIFO;
            case HIFO -> TaxLotMatchMethod.HIFO;
            default -> TaxLotMatchMethod.FIFO;
        };
        TaxLotMatcherResult engine = match(req);
        MatchResult legacy = new MatchResult();
        legacy.status = engine.status;
        legacy.unmatchedQtyMilli = engine.remainingQty;
        legacy.realizedGainCents = engine.totalGainLossCents;
        for (TaxLotMatchPair pair : engine.matches) {
            TaxLot consumed = new TaxLot();
            consumed.lotId = pair.lotId;
            consumed.qtyMilli = pair.matchedQtyMilli;
            consumed.costBasisCents = pair.costBasisCents;
            legacy.consumedLots.add(consumed);
        }
        return legacy;
    }

    public int openLotCount(int accountId, int symbolId) {
        int n = 0;
        for (TaxLot lot : openLots) {
            if (lot.accountId == accountId && lot.symbolId == symbolId && lot.qtyMilli > 0 && !lot.closed) n++;
        }
        return n;
    }

    public List<TaxLot> openLotsFor(int accountId, int symbolId) {
        List<TaxLot> result = new ArrayList<>();
        for (TaxLot lot : openLots) {
            if (lot.accountId == accountId && lot.symbolId == symbolId && !lot.closed) result.add(lot);
        }
        return result;
    }

    public long totalOpenCostBasis(int accountId, int symbolId) {
        long sum = 0;
        for (TaxLot lot : openLots) {
            if (lot.accountId == accountId && lot.symbolId == symbolId && !lot.closed) sum += lot.costBasisCents;
        }
        return sum;
    }
}
