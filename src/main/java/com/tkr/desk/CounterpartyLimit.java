package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Counterparty exposure limits with notional, quantity, turnover checks.
 * Ported from legacy-cpp counterparty_limit.cc.
 */
public final class CounterpartyLimit {

    public enum LimitKind { NOTIONAL, QUANTITY, DAILY_TURNOVER, NET_EXPOSURE, OPEN_ORDERS }

    public enum LimitScope { PER_COUNTERPARTY, PER_SYMBOL, PER_ACCOUNT, DESK_WIDE }

    public static final class CounterpartyProfile {
        public int counterpartyId;
        public String name = "";
        public int creditRatingBp;
        public boolean approved = true;
        public int parentId;
    }

    public static final class ExposureLimit {
        public LimitKind kind;
        public LimitScope scope;
        public int limitId;
        public int counterpartyId;
        public int symbolId;
        public int accountId;
        public long maxValue;
        public long warningThreshold;
        public boolean hardBlock = true;
    }

    public static final class ExposureReading {
        public int counterpartyId;
        public int accountId;
        public int symbolId;
        public long notionalCents;
        public long qtyMilli;
        public int openOrderCount;
        public long dailyTurnoverCents;
        public long netExposureCents;
    }

    public static final class LimitBreach {
        public LimitKind kind;
        public int limitId;
        public int counterpartyId;
        public int recordId;
        public long currentValue;
        public long limitValue;
        public boolean isWarning;
        public String reason = "";
    }

    public static final class CounterpartyLimitConfig {
        public int deskId;
        public boolean enforceHardBlocks;
        public boolean aggregateParent;

        public CounterpartyLimitConfig(int deskId, boolean enforceHardBlocks) {
            this.deskId = deskId;
            this.enforceHardBlocks = enforceHardBlocks;
            this.aggregateParent = false;
        }
    }

    public static final class CounterpartyLimitResult {
        public Status status = Status.OK;
        public List<LimitBreach> breaches = new ArrayList<>();
        public List<ExposureReading> readings = new ArrayList<>();
        public int breachCount;
        public int warningCount;
    }

    private static final int COUNTERPARTY_BASE = 50000;

    private final CounterpartyLimitConfig config;
    private final Map<Integer, CounterpartyProfile> counterparties = new HashMap<>();
    private final List<ExposureLimit> limits = new ArrayList<>();
    private final Map<Integer, ExposureReading> runningExposure = new HashMap<>();

    public CounterpartyLimit() {
        this(new CounterpartyLimitConfig(0, true));
    }

    public CounterpartyLimit(CounterpartyLimitConfig config) {
        this.config = config != null ? config : new CounterpartyLimitConfig(0, true);
        loadDefaultLimits();
    }

    public void loadDefaultLimits() {
        CounterpartyProfile brokerA = new CounterpartyProfile();
        brokerA.counterpartyId = 50001;
        brokerA.name = "BrokerAlpha";
        brokerA.creditRatingBp = 8500;
        brokerA.approved = true;
        registerCounterparty(brokerA);

        CounterpartyProfile brokerB = new CounterpartyProfile();
        brokerB.counterpartyId = 50002;
        brokerB.name = "BrokerBeta";
        brokerB.creditRatingBp = 7200;
        brokerB.approved = true;
        registerCounterparty(brokerB);

        ExposureLimit notional = new ExposureLimit();
        notional.kind = LimitKind.NOTIONAL;
        notional.scope = LimitScope.PER_COUNTERPARTY;
        notional.limitId = 6001;
        notional.maxValue = 500_000_000L;
        notional.warningThreshold = 400_000_000L;
        notional.hardBlock = true;
        setLimit(notional);

        ExposureLimit qty = new ExposureLimit();
        qty.kind = LimitKind.QUANTITY;
        qty.scope = LimitScope.PER_SYMBOL;
        qty.limitId = 6002;
        qty.maxValue = 10_000_000L;
        qty.warningThreshold = 8_000_000L;
        qty.hardBlock = true;
        setLimit(qty);

        ExposureLimit turnover = new ExposureLimit();
        turnover.kind = LimitKind.DAILY_TURNOVER;
        turnover.scope = LimitScope.PER_COUNTERPARTY;
        turnover.limitId = 6003;
        turnover.maxValue = 1_000_000_000L;
        turnover.warningThreshold = 800_000_000L;
        turnover.hardBlock = false;
        setLimit(turnover);
    }

    public void registerCounterparty(CounterpartyProfile profile) {
        counterparties.put(profile.counterpartyId, profile);
    }

    public void setLimit(ExposureLimit limit) {
        for (int i = 0; i < limits.size(); i++) {
            if (limits.get(i).limitId == limit.limitId) {
                limits.set(i, limit);
                return;
            }
        }
        limits.add(limit);
    }

    public void removeLimit(int limitId) {
        limits.removeIf(l -> l.limitId == limitId);
    }

    public void clearLimits() {
        limits.clear();
        runningExposure.clear();
    }

    public static long computeRecordNotional(WireBatchRecord rec) {
        return (long) rec.qtyMilli * rec.priceTick / 1000L;
    }

    public static int deriveCounterpartyId(int accountId) {
        return COUNTERPARTY_BASE + (accountId % 100);
    }

    public boolean isCounterpartyApproved(int counterpartyId) {
        CounterpartyProfile profile = counterparties.get(counterpartyId);
        return profile == null || profile.approved;
    }

    public long lookupCurrentExposure(int counterpartyId, LimitKind kind) {
        ExposureReading reading = runningExposure.get(counterpartyId);
        if (reading == null) return 0;
        return switch (kind) {
            case NOTIONAL -> reading.notionalCents;
            case QUANTITY -> reading.qtyMilli;
            case DAILY_TURNOVER -> reading.dailyTurnoverCents;
            case NET_EXPOSURE -> reading.netExposureCents;
            default -> 0;
        };
    }

    public ExposureReading buildReading(WireBatchRecord rec) {
        ExposureReading reading = new ExposureReading();
        reading.counterpartyId = deriveCounterpartyId(rec.accountId);
        reading.accountId = rec.accountId;
        reading.symbolId = rec.symbolId;
        reading.notionalCents = computeRecordNotional(rec);
        reading.qtyMilli = rec.qtyMilli;
        reading.openOrderCount = 1;
        reading.dailyTurnoverCents = reading.notionalCents;
        reading.netExposureCents = reading.notionalCents;
        return reading;
    }

    public ExposureReading aggregateReadings(List<ExposureReading> readings, int counterpartyId) {
        ExposureReading agg = new ExposureReading();
        agg.counterpartyId = counterpartyId;
        for (ExposureReading r : readings) {
            if (r.counterpartyId != counterpartyId) continue;
            agg.notionalCents += r.notionalCents;
            agg.qtyMilli += r.qtyMilli;
            agg.openOrderCount += r.openOrderCount;
            agg.dailyTurnoverCents += r.dailyTurnoverCents;
            agg.netExposureCents += r.netExposureCents;
        }
        return agg;
    }

    public List<ExposureLimit> limitsForCounterparty(int counterpartyId) {
        List<ExposureLimit> result = new ArrayList<>();
        for (ExposureLimit limit : limits) {
            if (limit.counterpartyId == 0 || limit.counterpartyId == counterpartyId) result.add(limit);
        }
        return result;
    }

    public Status checkLimit(ExposureLimit limit, ExposureReading reading, int recordId, LimitBreach out) {
        if (out == null) return Status.BOUNDS_ERROR;
        long current = switch (limit.kind) {
            case NOTIONAL -> reading.notionalCents;
            case QUANTITY -> reading.qtyMilli;
            case DAILY_TURNOVER -> reading.dailyTurnoverCents;
            case NET_EXPOSURE -> reading.netExposureCents;
            case OPEN_ORDERS -> reading.openOrderCount;
        };
        if (limit.scope == LimitScope.PER_SYMBOL && limit.symbolId != 0 && limit.symbolId != reading.symbolId) {
            return Status.OK;
        }
        if (limit.scope == LimitScope.PER_ACCOUNT && limit.accountId != 0 && limit.accountId != reading.accountId) {
            return Status.OK;
        }
        long prior = lookupCurrentExposure(reading.counterpartyId, limit.kind);
        current += prior;
        if (current > limit.maxValue) {
            out.kind = limit.kind;
            out.limitId = limit.limitId;
            out.counterpartyId = reading.counterpartyId;
            out.recordId = recordId;
            out.currentValue = current;
            out.limitValue = limit.maxValue;
            out.isWarning = false;
            out.reason = "limit breach: current " + current + " exceeds max " + limit.maxValue;
            return limit.hardBlock ? Status.COMPLIANCE_REJECT : Status.OK;
        }
        if (current > limit.warningThreshold) {
            out.kind = limit.kind;
            out.limitId = limit.limitId;
            out.counterpartyId = reading.counterpartyId;
            out.recordId = recordId;
            out.currentValue = current;
            out.limitValue = limit.warningThreshold;
            out.isWarning = true;
            out.reason = "limit warning: approaching threshold";
        }
        return Status.OK;
    }

    public CounterpartyLimitResult evaluateReading(ExposureReading reading) {
        CounterpartyLimitResult result = new CounterpartyLimitResult();
        result.status = Status.OK;
        if (!isCounterpartyApproved(reading.counterpartyId)) {
            LimitBreach breach = new LimitBreach();
            breach.kind = LimitKind.NOTIONAL;
            breach.counterpartyId = reading.counterpartyId;
            breach.reason = "counterparty not approved";
            result.breaches.add(breach);
            result.breachCount++;
            result.status = Status.COMPLIANCE_REJECT;
            return result;
        }
        for (ExposureLimit limit : limitsForCounterparty(reading.counterpartyId)) {
            LimitBreach breach = new LimitBreach();
            Status st = checkLimit(limit, reading, 0, breach);
            if (st == Status.COMPLIANCE_REJECT) {
                result.breaches.add(breach);
                result.breachCount++;
                if (config.enforceHardBlocks) result.status = Status.COMPLIANCE_REJECT;
            } else if (breach.isWarning) {
                result.breaches.add(breach);
                result.warningCount++;
            }
        }
        ExposureReading running = runningExposure.computeIfAbsent(reading.counterpartyId, id -> new ExposureReading());
        running.counterpartyId = reading.counterpartyId;
        running.notionalCents += reading.notionalCents;
        running.qtyMilli += reading.qtyMilli;
        running.dailyTurnoverCents += reading.dailyTurnoverCents;
        running.netExposureCents += reading.netExposureCents;
        running.openOrderCount += reading.openOrderCount;
        result.readings.add(reading);
        return result;
    }

    public CounterpartyLimitResult evaluate(BatchWireFrame frame) {
        CounterpartyLimitResult result = new CounterpartyLimitResult();
        result.status = Status.OK;
        if (frame == null) return result;
        for (WireBatchRecord rec : frame.records) {
            ExposureReading reading = buildReading(rec);
            CounterpartyLimitResult partial = evaluateReading(reading);
            for (LimitBreach b : partial.breaches) {
                result.breaches.add(b);
                if (b.isWarning) result.warningCount++;
                else result.breachCount++;
            }
            if (partial.status == Status.COMPLIANCE_REJECT && config.enforceHardBlocks) {
                result.status = Status.COMPLIANCE_REJECT;
            }
            result.readings.add(reading);
        }
        if (result.breachCount > 0 && config.enforceHardBlocks) result.status = Status.COMPLIANCE_REJECT;
        return result;
    }

    public void resetRunningExposure() {
        runningExposure.clear();
    }

    public long totalDeskExposure(LimitKind kind) {
        long total = 0;
        for (ExposureReading reading : runningExposure.values()) {
            total += switch (kind) {
                case NOTIONAL -> reading.notionalCents;
                case QUANTITY -> reading.qtyMilli;
                case DAILY_TURNOVER -> reading.dailyTurnoverCents;
                default -> 0;
            };
        }
        return total;
    }

    public CounterpartyLimitResult evaluateParentAggregate(BatchWireFrame frame) {
        CounterpartyLimitResult result = new CounterpartyLimitResult();
        result.status = Status.OK;
        if (!config.aggregateParent) return evaluate(frame);
        Map<Integer, List<ExposureReading>> byParent = new HashMap<>();
        for (WireBatchRecord rec : frame.records) {
            ExposureReading reading = buildReading(rec);
            int parentId = reading.counterpartyId;
            CounterpartyProfile profile = counterparties.get(reading.counterpartyId);
            if (profile != null && profile.parentId != 0) parentId = profile.parentId;
            byParent.computeIfAbsent(parentId, k -> new ArrayList<>()).add(reading);
        }
        for (Map.Entry<Integer, List<ExposureReading>> entry : byParent.entrySet()) {
            ExposureReading agg = aggregateReadings(entry.getValue(), entry.getKey());
            CounterpartyLimitResult partial = evaluateReading(agg);
            for (LimitBreach b : partial.breaches) {
                result.breaches.add(b);
                if (b.isWarning) result.warningCount++;
                else result.breachCount++;
            }
            result.readings.add(agg);
        }
        if (result.breachCount > 0 && config.enforceHardBlocks) result.status = Status.COMPLIANCE_REJECT;
        return result;
    }

    public List<LimitBreach> breachesForCounterparty(CounterpartyLimitResult result, int counterpartyId) {
        List<LimitBreach> filtered = new ArrayList<>();
        for (LimitBreach b : result.breaches) {
            if (b.counterpartyId == counterpartyId) filtered.add(b);
        }
        return filtered;
    }

    public long exposureUtilizationBp(int counterpartyId, LimitKind kind) {
        List<ExposureLimit> applicable = limitsForCounterparty(counterpartyId);
        long current = lookupCurrentExposure(counterpartyId, kind);
        for (ExposureLimit limit : applicable) {
            if (limit.kind == kind && limit.maxValue > 0) {
                return current * 10000L / limit.maxValue;
            }
        }
        return 0;
    }

    public boolean isWithinWarningThreshold(int counterpartyId, LimitKind kind) {
        long util = exposureUtilizationBp(counterpartyId, kind);
        return util > 8000 && util <= 10000;
    }

    public String formatBreach(LimitBreach breach) {
        return breach.kind + " cp=" + breach.counterpartyId + " current=" + breach.currentValue
                + " limit=" + breach.limitValue + " warn=" + breach.isWarning;
    }
}
