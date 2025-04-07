package com.tkr.desk;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Margin aggregator with SPAN scanning risk. Ported from legacy-cpp margin_aggregator.cc.
 */
public final class MarginAggregator {

    public static final class MarginLine {
        public int accountId;
        public int symbolId;
        public int qtyMilli;
        public long notionalCents;
        public long initialMarginCents;
        public long maintenanceMarginCents;
    }

    public static final class MarginAggregatorConfig {
        public int deskId;
        public int marginRateBp;

        public MarginAggregatorConfig(int deskId, int marginRateBp) {
            this.deskId = deskId;
            this.marginRateBp = marginRateBp;
        }
    }

    public static final class MarginAggregatorResult {
        public Status status = Status.OK;
        public List<MarginLine> lines = new ArrayList<>();
        public List<MarginCheckResult> accountChecks = new ArrayList<>();
        public long totalInitialMarginCents;
        public int breachCount;
    }

    /** Legacy wrapper for DeskRegistry. */
    public static final class AccountMarginState {
        public int accountId;
        public long requiredMarginCents;
        public long availableMarginCents;
        public boolean passed;
    }

    public static final class MarginBatchResult {
        public Status status = Status.OK;
        public List<AccountMarginState> accounts = new ArrayList<>();
        public boolean allPassed = true;
    }

    private static final class AccountRollup {
        int accountId;
        long initialMarginCents;
        long maintenanceMarginCents;
        long notionalCents;
        int lineCount;
    }

    private static final class SymbolExposure {
        int symbolId;
        long netQtyMilli;
        long grossNotionalCents;
        long spanScanningRiskCents;
    }

    private static final class SpanScenario {
        int priceShiftBp;
        int volShiftBp;
        long pnlCents;

        SpanScenario(int priceShiftBp, int volShiftBp, long pnlCents) {
            this.priceShiftBp = priceShiftBp;
            this.volShiftBp = volShiftBp;
            this.pnlCents = pnlCents;
        }
    }

    private static final long DEFAULT_AVAILABLE_CENTS = 1_000_000_000L;
    private static final int MAINT_RATIO_BP = 7500;
    private static final int DEFAULT_MARGIN_RATE_BP = 1500;

    private final MarginAggregatorConfig config;
    private final Map<Integer, Long> accountAvailable = new HashMap<>();

    public MarginAggregator() {
        this(new MarginAggregatorConfig(0, DEFAULT_MARGIN_RATE_BP));
    }

    public MarginAggregator(MarginAggregatorConfig config) {
        this.config = config != null ? config : new MarginAggregatorConfig(0, DEFAULT_MARGIN_RATE_BP);
    }

    public void setAvailableMarginCents(int accountId, long cents) {
        accountAvailable.put(accountId, cents);
    }

    public void clearAccountMargins() {
        accountAvailable.clear();
    }

    public static long computeNotionalCents(int qtyMilli, int priceTick) {
        return (long) qtyMilli * (long) priceTick / 1000L;
    }

    public static long computeInitialMargin(long notionalCents, int rateBp) {
        if (notionalCents <= 0) return 0;
        return notionalCents * rateBp / 10000L;
    }

    public static long computeMaintenanceMargin(long initialMargin, int maintRatioBp) {
        if (initialMargin <= 0) return 0;
        return initialMargin * maintRatioBp / 10000L;
    }

    private long lookupAvailableMargin(int accountId) {
        return accountAvailable.getOrDefault(accountId, DEFAULT_AVAILABLE_CENTS);
    }

    private static long applySpanPriceScan(long notionalCents, int shiftBp) {
        return notionalCents * shiftBp / 10000L;
    }

    private static long computeSpanScanningRisk(SymbolExposure exposure) {
        SpanScenario[] scenarios = {
            new SpanScenario(500, 0, applySpanPriceScan(exposure.grossNotionalCents, 500)),
            new SpanScenario(-500, 0, applySpanPriceScan(exposure.grossNotionalCents, -500)),
            new SpanScenario(300, 200, applySpanPriceScan(exposure.grossNotionalCents, 300)),
            new SpanScenario(-300, 200, applySpanPriceScan(exposure.grossNotionalCents, -300)),
            new SpanScenario(0, 500, applySpanPriceScan(exposure.grossNotionalCents, 250))
        };
        long worstLoss = 0;
        for (SpanScenario sc : scenarios) {
            if (sc.pnlCents < worstLoss) worstLoss = sc.pnlCents;
        }
        return -worstLoss;
    }

    public Status buildMarginLines(BatchWireFrame frame, List<MarginLine> lines) {
        if (lines == null) return Status.BOUNDS_ERROR;
        lines.clear();
        Map<Integer, SymbolExposure> exposures = new HashMap<>();
        for (WireBatchRecord rec : frame.records) {
            MarginLine line = new MarginLine();
            line.accountId = rec.accountId;
            line.symbolId = rec.symbolId;
            line.qtyMilli = rec.qtyMilli;
            line.notionalCents = computeNotionalCents(rec.qtyMilli, rec.priceTick);
            SymbolExposure exp = exposures.computeIfAbsent(rec.symbolId, id -> {
                SymbolExposure s = new SymbolExposure();
                s.symbolId = id;
                return s;
            });
            exp.netQtyMilli += rec.qtyMilli;
            exp.grossNotionalCents += line.notionalCents;
            line.initialMarginCents = computeInitialMargin(line.notionalCents, config.marginRateBp);
            line.maintenanceMarginCents = computeMaintenanceMargin(line.initialMarginCents, MAINT_RATIO_BP);
            lines.add(line);
        }
        for (MarginLine line : lines) {
            SymbolExposure exp = exposures.get(line.symbolId);
            if (exp == null) continue;
            long spanRisk = computeSpanScanningRisk(exp);
            if (spanRisk > line.initialMarginCents) {
                line.initialMarginCents = spanRisk;
                line.maintenanceMarginCents = computeMaintenanceMargin(spanRisk, MAINT_RATIO_BP);
            }
        }
        return Status.OK;
    }

    public Status rollUpByAccount(List<MarginLine> lines, List<MarginCheckResult> checks) {
        if (checks == null) return Status.BOUNDS_ERROR;
        Map<Integer, AccountRollup> rollup = new HashMap<>();
        for (MarginLine line : lines) {
            AccountRollup acc = rollup.computeIfAbsent(line.accountId, id -> {
                AccountRollup a = new AccountRollup();
                a.accountId = id;
                return a;
            });
            acc.initialMarginCents += line.initialMarginCents;
            acc.maintenanceMarginCents += line.maintenanceMarginCents;
            acc.notionalCents += line.notionalCents;
            acc.lineCount++;
        }
        checks.clear();
        for (AccountRollup acc : rollup.values()) {
            MarginCheckResult check = new MarginCheckResult();
            check.accountId = acc.accountId;
            check.requiredMarginCents = acc.initialMarginCents;
            check.availableMarginCents = lookupAvailableMargin(acc.accountId);
            check.passed = check.availableMarginCents >= check.requiredMarginCents;
            checks.add(check);
        }
        checks.sort(Comparator.comparingInt(c -> c.accountId));
        return Status.OK;
    }

    public MarginAggregatorResult aggregate(BatchWireFrame frame) {
        MarginAggregatorResult result = new MarginAggregatorResult();
        result.status = Status.OK;
        if (frame == null || frame.records.isEmpty()) return result;
        Status buildSt = buildMarginLines(frame, result.lines);
        if (buildSt != Status.OK) {
            result.status = buildSt;
            return result;
        }
        for (MarginLine line : result.lines) result.totalInitialMarginCents += line.initialMarginCents;
        Status rollSt = rollUpByAccount(result.lines, result.accountChecks);
        if (rollSt != Status.OK) {
            result.status = rollSt;
            return result;
        }
        for (MarginCheckResult check : result.accountChecks) {
            if (!check.passed) result.breachCount++;
        }
        if (result.breachCount > 0 && (frame.header.flags & WireTypes.BATCH_FLAG_MARGIN_CHECK) != 0) {
            result.status = Status.MARGIN_BREACH;
        }
        return result;
    }

    public long computeRequiredMarginCents(int qtyMilli, int priceTick) {
        return computeInitialMargin(computeNotionalCents(qtyMilli, priceTick), config.marginRateBp);
    }

    /** Legacy API. */
    public MarginBatchResult aggregateBatch(BatchWireFrame frame) {
        MarginAggregatorResult agg = aggregate(frame);
        MarginBatchResult result = new MarginBatchResult();
        result.status = agg.status;
        result.allPassed = agg.breachCount == 0;
        for (MarginCheckResult check : agg.accountChecks) {
            AccountMarginState state = new AccountMarginState();
            state.accountId = check.accountId;
            state.requiredMarginCents = check.requiredMarginCents;
            state.availableMarginCents = check.availableMarginCents;
            state.passed = check.passed;
            result.accounts.add(state);
            if (!state.passed) result.allPassed = false;
        }
        if (!result.allPassed && result.status == Status.OK) result.status = Status.MARGIN_BREACH;
        return result;
    }

    public MarginCheckResult toCheckResult(AccountMarginState state) {
        MarginCheckResult r = new MarginCheckResult();
        r.accountId = state.accountId;
        r.requiredMarginCents = state.requiredMarginCents;
        r.availableMarginCents = state.availableMarginCents;
        r.passed = state.passed;
        return r;
    }

    public long totalRequiredCents(MarginBatchResult result) {
        long sum = 0;
        for (AccountMarginState s : result.accounts) sum += s.requiredMarginCents;
        return sum;
    }

    public AccountMarginState worstAccount(MarginBatchResult result) {
        AccountMarginState worst = null;
        for (AccountMarginState s : result.accounts) {
            if (!s.passed && (worst == null || s.requiredMarginCents > worst.requiredMarginCents)) worst = s;
        }
        return worst;
    }

    public long totalMaintenanceCents(MarginAggregatorResult result) {
        long sum = 0;
        for (MarginLine line : result.lines) sum += line.maintenanceMarginCents;
        return sum;
    }

    public List<MarginLine> linesForAccount(MarginAggregatorResult result, int accountId) {
        List<MarginLine> filtered = new ArrayList<>();
        for (MarginLine line : result.lines) {
            if (line.accountId == accountId) filtered.add(line);
        }
        return filtered;
    }

    public long spanRiskForSymbol(BatchWireFrame frame, int symbolId) {
        SymbolExposure exp = new SymbolExposure();
        exp.symbolId = symbolId;
        for (WireBatchRecord rec : frame.records) {
            if (rec.symbolId != symbolId) continue;
            exp.netQtyMilli += rec.qtyMilli;
            exp.grossNotionalCents += computeNotionalCents(rec.qtyMilli, rec.priceTick);
        }
        return computeSpanScanningRisk(exp);
    }

    public boolean passesMarginCheck(int accountId, long requiredCents) {
        return lookupAvailableMargin(accountId) >= requiredCents;
    }

    public Map<Integer, Long> snapshotAvailable() {
        return new HashMap<>(accountAvailable);
    }

    public int marginRateBp() {
        return config.marginRateBp;
    }

    public String summarizeResult(MarginAggregatorResult result) {
        return "lines=" + result.lines.size() + " breaches=" + result.breachCount
                + " totalInitial=" + result.totalInitialMarginCents;
    }

    public MarginCheckResult worstBreach(MarginAggregatorResult result) {
        MarginCheckResult worst = null;
        for (MarginCheckResult check : result.accountChecks) {
            if (!check.passed && (worst == null || check.requiredMarginCents > worst.requiredMarginCents)) {
                worst = check;
            }
        }
        return worst;
    }

    public long headroomCents(int accountId, MarginAggregatorResult result) {
        for (MarginCheckResult check : result.accountChecks) {
            if (check.accountId == accountId) {
                return check.availableMarginCents - check.requiredMarginCents;
            }
        }
        return lookupAvailableMargin(accountId);
    }

    public int breachAccountCount(MarginAggregatorResult result) {
        return result.breachCount;
    }
}
