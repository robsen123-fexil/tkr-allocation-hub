package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Tiered fee accrual engine with flat, notional, per-share schedules.
 * Ported from legacy-cpp fee_accrual_engine.cc.
 */
public final class FeeAccrualEngine {

    public enum FeeScheduleKind {
        FLAT_PER_TRADE, TIERED_NOTIONAL, PER_SHARE_MILLI, PERFORMANCE_FEE, MONTHLY_MINIMUM
    }

    public static final class FeeTier {
        public long notionalFloorCents;
        public long notionalCeilingCents;
        public int rateBp;
        public long flatFeeCents;
    }

    public static final class FeeSchedule {
        public FeeScheduleKind kind;
        public int scheduleId;
        public int accountId;
        public List<FeeTier> tiers = new ArrayList<>();
        public int perShareMilliCents;
        public long monthlyMinimumCents;
    }

    public static final class AccrualEntry {
        public int accountId;
        public int scheduleId;
        public long accruedCents;
        public long notionalBasisCents;
        public int tradeCount;
        public int accrualDateYyyymmdd;
    }

    public static final class FeeAccrualEngineConfig {
        public int deskId;
        public int accrualDateYyyymmdd;

        public FeeAccrualEngineConfig(int deskId, int accrualDateYyyymmdd) {
            this.deskId = deskId;
            this.accrualDateYyyymmdd = accrualDateYyyymmdd;
        }
    }

    public static final class FeeAccrualEngineResult {
        public Status status = Status.OK;
        public long totalAccruedCents;
        public int schedulesApplied;
        public List<AccrualEntry> entries = new ArrayList<>();
    }

    private static final class FeeBreakdown {
        long commissionCents;
        long exchangeCents;
        long clearingCents;
        long regulatoryCents;
    }

    private final FeeAccrualEngineConfig config;
    private final List<FeeSchedule> schedules = new ArrayList<>();
    private final List<AccrualEntry> ledger = new ArrayList<>();

    public FeeAccrualEngine() {
        this(new FeeAccrualEngineConfig(0, 0));
    }

    public FeeAccrualEngine(FeeAccrualEngineConfig config) {
        this.config = config != null ? config : new FeeAccrualEngineConfig(0, 0);
        loadDefaultSchedules();
    }

    private static FeeBreakdown computeFeeBreakdown(long notionalCents, int qtyMilli) {
        FeeBreakdown bd = new FeeBreakdown();
        bd.commissionCents = notionalCents * 5 / 10000;
        bd.exchangeCents = notionalCents / 10000;
        bd.clearingCents = (long) qtyMilli * 2 / 1000;
        bd.regulatoryCents = notionalCents * 2 / 10000;
        return bd;
    }

    public void loadDefaultSchedules() {
        schedules.clear();
        FeeSchedule flat = new FeeSchedule();
        flat.kind = FeeScheduleKind.FLAT_PER_TRADE;
        flat.scheduleId = 1;
        FeeTier flatTier = new FeeTier();
        flatTier.notionalFloorCents = 0;
        flatTier.notionalCeilingCents = Long.MAX_VALUE;
        flatTier.flatFeeCents = 250;
        flat.tiers.add(flatTier);
        schedules.add(flat);

        FeeSchedule tiered = new FeeSchedule();
        tiered.kind = FeeScheduleKind.TIERED_NOTIONAL;
        tiered.scheduleId = 2;
        tiered.tiers.add(tier(0, 10_000_000L, 10, 0));
        tiered.tiers.add(tier(10_000_000L, 100_000_000L, 5, 0));
        tiered.tiers.add(tier(100_000_000L, Long.MAX_VALUE, 2, 0));
        schedules.add(tiered);

        FeeSchedule perShare = new FeeSchedule();
        perShare.kind = FeeScheduleKind.PER_SHARE_MILLI;
        perShare.scheduleId = 3;
        perShare.perShareMilliCents = 3;
        schedules.add(perShare);
    }

    private static FeeTier tier(long floor, long ceiling, int rateBp, long flat) {
        FeeTier t = new FeeTier();
        t.notionalFloorCents = floor;
        t.notionalCeilingCents = ceiling;
        t.rateBp = rateBp;
        t.flatFeeCents = flat;
        return t;
    }

    public void registerSchedule(FeeSchedule schedule) {
        if (schedule != null) schedules.add(schedule);
    }

    public void clearSchedules() {
        schedules.clear();
        ledger.clear();
    }

    public long accrueFlatPerTrade(FeeSchedule schedule) {
        return schedule.tiers.isEmpty() ? 0 : schedule.tiers.get(0).flatFeeCents;
    }

    public long accrueTieredNotional(FeeSchedule schedule, long notionalCents) {
        for (FeeTier tier : schedule.tiers) {
            if (notionalCents >= tier.notionalFloorCents && notionalCents < tier.notionalCeilingCents) {
                return notionalCents * tier.rateBp / 10000L + tier.flatFeeCents;
            }
        }
        return 0;
    }

    public long accruePerShareMilli(FeeSchedule schedule, int qtyMilli) {
        return (long) qtyMilli * schedule.perShareMilliCents / 1000L;
    }

    public long accrueMonthlyMinimum(FeeSchedule schedule, long runningTotal) {
        return runningTotal >= schedule.monthlyMinimumCents ? 0 : schedule.monthlyMinimumCents - runningTotal;
    }

    public long computeTradeFee(FeeSchedule schedule, int qtyMilli, long notionalCents) {
        long baseFee = switch (schedule.kind) {
            case FLAT_PER_TRADE -> accrueFlatPerTrade(schedule);
            case TIERED_NOTIONAL -> accrueTieredNotional(schedule, notionalCents);
            case PER_SHARE_MILLI -> accruePerShareMilli(schedule, qtyMilli);
            case PERFORMANCE_FEE -> notionalCents * 2000 / 10000L;
            case MONTHLY_MINIMUM -> schedule.monthlyMinimumCents;
        };
        FeeBreakdown bd = computeFeeBreakdown(notionalCents, qtyMilli);
        return baseFee + bd.exchangeCents + bd.clearingCents + bd.regulatoryCents;
    }

    public void upsertLedger(int accountId, int scheduleId, long feeCents, long notionalCents) {
        for (AccrualEntry entry : ledger) {
            if (entry.accountId == accountId && entry.scheduleId == scheduleId) {
                entry.accruedCents += feeCents;
                entry.notionalBasisCents += notionalCents;
                entry.tradeCount++;
                return;
            }
        }
        AccrualEntry entry = new AccrualEntry();
        entry.accountId = accountId;
        entry.scheduleId = scheduleId;
        entry.accruedCents = feeCents;
        entry.notionalBasisCents = notionalCents;
        entry.tradeCount = 1;
        entry.accrualDateYyyymmdd = config.accrualDateYyyymmdd;
        ledger.add(entry);
    }

    public FeeAccrualEngineResult accrueBatch(BatchWireFrame frame) {
        FeeAccrualEngineResult result = new FeeAccrualEngineResult();
        result.status = Status.OK;
        if (frame == null) return result;
        for (WireBatchRecord rec : frame.records) {
            long notional = (long) rec.qtyMilli * rec.priceTick / 1000L;
            for (FeeSchedule schedule : schedules) {
                if (schedule.accountId != 0 && schedule.accountId != rec.accountId) continue;
                long fee = computeTradeFee(schedule, rec.qtyMilli, notional);
                if (schedule.kind == FeeScheduleKind.MONTHLY_MINIMUM) {
                    long running = 0;
                    for (AccrualEntry entry : ledger) {
                        if (entry.accountId == rec.accountId && entry.scheduleId == schedule.scheduleId) {
                            running = entry.accruedCents;
                            break;
                        }
                    }
                    long minTopUp = accrueMonthlyMinimum(schedule, running + fee);
                    upsertLedger(rec.accountId, schedule.scheduleId, fee + minTopUp, notional);
                    result.totalAccruedCents += fee + minTopUp;
                } else {
                    upsertLedger(rec.accountId, schedule.scheduleId, fee, notional);
                    result.totalAccruedCents += fee;
                }
                result.schedulesApplied++;
            }
        }
        result.entries = new ArrayList<>(ledger);
        result.entries.sort(Comparator.comparingInt((AccrualEntry a) -> a.accountId).thenComparingInt(a -> a.scheduleId));
        return result;
    }

    public List<AccrualEntry> ledgerSnapshot() {
        return new ArrayList<>(ledger);
    }

    public long totalForAccount(int accountId) {
        long sum = 0;
        for (AccrualEntry e : ledger) if (e.accountId == accountId) sum += e.accruedCents;
        return sum;
    }
}
