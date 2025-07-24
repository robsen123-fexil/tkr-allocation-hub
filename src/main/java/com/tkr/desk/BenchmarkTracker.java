package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Benchmark tracker with active weights and tracking error computation.
 * Ported from legacy-cpp benchmark_tracker.cc.
 */
public final class BenchmarkTracker {

    public static final class BenchmarkConstituent {
        public int symbolId;
        public int weightBp;
        public long benchmarkReturnBp;
    }

    public static final class PortfolioHolding {
        public int accountId;
        public int symbolId;
        public long qtyMilli;
        public long marketValueCents;
    }

    public static final class TrackingErrorEntry {
        public int symbolId;
        public long benchmarkWeightBp;
        public long portfolioWeightBp;
        public long activeWeightBp;
        public long contributionBp;
    }

    public static final class BenchmarkTrackerConfig {
        public int benchmarkId;
        public int rebalanceThresholdBp;

        public BenchmarkTrackerConfig(int benchmarkId, int rebalanceThresholdBp) {
            this.benchmarkId = benchmarkId;
            this.rebalanceThresholdBp = rebalanceThresholdBp;
        }
    }

    public static final class BenchmarkTrackerResult {
        public Status status = Status.OK;
        public List<TrackingErrorEntry> constituents = new ArrayList<>();
        public long portfolioReturnBp;
        public long benchmarkReturnBp;
        public long activeReturnBp;
        public long trackingErrorBp;
        public boolean rebalanceNeeded;
    }

    private final BenchmarkTrackerConfig config;
    private List<BenchmarkConstituent> benchmark = new ArrayList<>();

    public BenchmarkTracker() {
        this(new BenchmarkTrackerConfig(1, 100));
    }

    public BenchmarkTracker(BenchmarkTrackerConfig config) {
        this.config = config != null ? config : new BenchmarkTrackerConfig(1, 100);
    }

    public void setBenchmark(List<BenchmarkConstituent> constituents) {
        benchmark = constituents != null ? new ArrayList<>(constituents) : new ArrayList<>();
    }

    public void clearBenchmark() {
        benchmark.clear();
    }

    public long lookupBenchmarkWeight(int symbolId) {
        for (BenchmarkConstituent c : benchmark) {
            if (c.symbolId == symbolId) return c.weightBp;
        }
        return 0;
    }

    public long lookupBenchmarkReturn(int symbolId) {
        for (BenchmarkConstituent c : benchmark) {
            if (c.symbolId == symbolId) return c.benchmarkReturnBp;
        }
        return 0;
    }

    public static long computePortfolioWeight(PortfolioHolding holding, long totalValueCents) {
        if (totalValueCents == 0) return 0;
        return holding.marketValueCents * 10000L / totalValueCents;
    }

    public static long computeWeightedReturn(List<PortfolioHolding> holdings, List<BenchmarkConstituent> benchmark) {
        long totalValue = 0;
        for (PortfolioHolding h : holdings) totalValue += h.marketValueCents;
        if (totalValue == 0) return 0;
        long weightedReturn = 0;
        for (PortfolioHolding h : holdings) {
            long weight = h.marketValueCents * 10000L / totalValue;
            long symbolReturn = 0;
            for (BenchmarkConstituent c : benchmark) {
                if (c.symbolId == h.symbolId) {
                    symbolReturn = c.benchmarkReturnBp;
                    break;
                }
            }
            weightedReturn += weight * symbolReturn / 10000L;
        }
        return weightedReturn;
    }

    public static long computeTrackingError(List<TrackingErrorEntry> entries) {
        long sumSq = 0;
        for (TrackingErrorEntry e : entries) {
            long diff = e.activeWeightBp;
            sumSq += diff * diff / 10000L;
        }
        long lo = 0;
        long hi = sumSq;
        while (lo < hi) {
            long mid = (lo + hi + 1) / 2;
            if (mid * mid <= sumSq) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

    public List<TrackingErrorEntry> buildActiveWeights(List<PortfolioHolding> holdings) {
        List<TrackingErrorEntry> entries = new ArrayList<>();
        long totalValue = 0;
        for (PortfolioHolding h : holdings) totalValue += h.marketValueCents;
        Map<Integer, Long> portfolioWeights = new HashMap<>();
        for (PortfolioHolding h : holdings) {
            portfolioWeights.put(h.symbolId, computePortfolioWeight(h, totalValue));
        }
        for (BenchmarkConstituent c : benchmark) {
            TrackingErrorEntry entry = new TrackingErrorEntry();
            entry.symbolId = c.symbolId;
            entry.benchmarkWeightBp = c.weightBp;
            entry.portfolioWeightBp = portfolioWeights.getOrDefault(c.symbolId, 0L);
            entry.activeWeightBp = entry.portfolioWeightBp - entry.benchmarkWeightBp;
            entry.contributionBp = entry.activeWeightBp * c.benchmarkReturnBp / 10000L;
            entries.add(entry);
        }
        for (PortfolioHolding h : holdings) {
            boolean inBenchmark = false;
            for (BenchmarkConstituent c : benchmark) {
                if (c.symbolId == h.symbolId) {
                    inBenchmark = true;
                    break;
                }
            }
            if (!inBenchmark) {
                TrackingErrorEntry entry = new TrackingErrorEntry();
                entry.symbolId = h.symbolId;
                entry.portfolioWeightBp = computePortfolioWeight(h, totalValue);
                entry.benchmarkWeightBp = 0;
                entry.activeWeightBp = entry.portfolioWeightBp;
                entries.add(entry);
            }
        }
        return entries;
    }

    public BenchmarkTrackerResult track(List<PortfolioHolding> holdings) {
        BenchmarkTrackerResult result = new BenchmarkTrackerResult();
        result.status = Status.OK;
        result.constituents = buildActiveWeights(holdings);
        result.portfolioReturnBp = computeWeightedReturn(holdings, benchmark);
        long benchReturn = 0;
        for (BenchmarkConstituent c : benchmark) {
            benchReturn += (long) c.weightBp * c.benchmarkReturnBp / 10000L;
        }
        result.benchmarkReturnBp = benchReturn;
        result.activeReturnBp = result.portfolioReturnBp - result.benchmarkReturnBp;
        result.trackingErrorBp = computeTrackingError(result.constituents);
        for (TrackingErrorEntry e : result.constituents) {
            if (e.activeWeightBp > config.rebalanceThresholdBp
                    || e.activeWeightBp < -config.rebalanceThresholdBp) {
                result.rebalanceNeeded = true;
                break;
            }
        }
        return result;
    }

    public BenchmarkTrackerResult trackBatch(BatchWireFrame frame) {
        List<PortfolioHolding> holdings = new ArrayList<>();
        for (WireBatchRecord rec : frame.records) {
            PortfolioHolding h = new PortfolioHolding();
            h.accountId = rec.accountId;
            h.symbolId = rec.symbolId;
            h.qtyMilli = rec.qtyMilli;
            h.marketValueCents = (long) rec.qtyMilli * rec.priceTick / 1000L;
            holdings.add(h);
        }
        return track(holdings);
    }

    public void addConstituent(int symbolId, int weightBp, long returnBp) {
        BenchmarkConstituent c = new BenchmarkConstituent();
        c.symbolId = symbolId;
        c.weightBp = weightBp;
        c.benchmarkReturnBp = returnBp;
        benchmark.add(c);
    }

    public List<BenchmarkConstituent> getBenchmark() {
        return new ArrayList<>(benchmark);
    }

    public long maxActiveWeightBp(BenchmarkTrackerResult result) {
        long max = 0;
        for (TrackingErrorEntry e : result.constituents) {
            long abs = Math.abs(e.activeWeightBp);
            if (abs > max) max = abs;
        }
        return max;
    }
}
