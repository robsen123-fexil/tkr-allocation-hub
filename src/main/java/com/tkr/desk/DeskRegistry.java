package com.tkr.desk;

import com.tkr.ledger.AuditSpool;
import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

/**
 * Registry orchestrating desk module passes on batch frames.
 * Ported from legacy-cpp desk_registry.cc.
 */
public final class DeskRegistry {

    public static final class DeskRunContext {
        public int batchId;
        public int recordCount;
        public int totalQtyMilli;
        public boolean marginOk;
        public boolean complianceOk;
        public boolean settlementOk;
        public boolean allocationOk;
    }

    private final ProRataAllocator proRata = new ProRataAllocator();
    private final AllocationSolver solver = new AllocationSolver();
    private final ComplianceRuleEngine compliance = new ComplianceRuleEngine();
    private final MarginAggregator margin = new MarginAggregator();
    private final HaircutCalculator haircut = new HaircutCalculator();
    private final CorporateActionAdjuster corpAction = new CorporateActionAdjuster();
    private final RestrictionFilter restrictions = new RestrictionFilter();
    private final FeeAccrualEngine fees = new FeeAccrualEngine();
    private final PositionLedger positions = new PositionLedger();
    private final CounterpartyLimit counterpartyLimit = new CounterpartyLimit();
    private final SettlementCalendar calendar = new SettlementCalendar();
    private final LotSplitter lotSplitter = new LotSplitter();
    private final TaxLotMatcher taxMatcher = new TaxLotMatcher();
    private final BenchmarkTracker benchmark = new BenchmarkTracker();

    public Status runBatchDesks(BatchWireFrame frame, DeskRunContext ctx) {
        if (frame == null || ctx == null) return Status.BOUNDS_ERROR;
        ctx.batchId = frame.header.deskId;
        ctx.recordCount = frame.records.size();
        long totalQty = 0;
        for (WireBatchRecord rec : frame.records) totalQty += rec.qtyMilli;
        ctx.totalQtyMilli = (int) totalQty;

        Status st = runCompliancePass(frame);
        ctx.complianceOk = st == Status.OK;
        if ((frame.header.flags & WireTypes.BATCH_FLAG_COMPLIANCE_HOLD) != 0 && !ctx.complianceOk) return st;

        CounterpartyLimit.CounterpartyLimitResult cpResult = counterpartyLimit.evaluate(frame);
        if (cpResult.status == Status.COMPLIANCE_REJECT) {
            ctx.complianceOk = false;
            return Status.COMPLIANCE_REJECT;
        }

        SettlementCalendar.SettlementCalendarResult settleResult = calendar.computeBatch(frame);
        ctx.settlementOk = settleResult.status == Status.OK;
        if (settleResult.status != Status.OK) return settleResult.status;

        LotSplitter.LotSplitterResult split = lotSplitter.splitBatch(frame, LotSplitter.LotSplitPolicy.PRO_RATA_BY_WEIGHT);
        if (split.status != Status.OK) return split.status;

        ProRataAllocator.ProRataAllocatorResult alloc = proRata.allocate(frame);
        ctx.allocationOk = alloc.status == Status.OK;
        if (alloc.status != Status.OK) return alloc.status;

        int targetTotal = 0;
        for (WireBatchRecord rec : frame.records) targetTotal += rec.qtyMilli;
        AllocationSolver.AllocationSolverResult solved = solver.solveWithBounds(alloc.slices, null, targetTotal);
        if (solved.status != Status.OK) return solved.status;

        Status ledgerSt = positions.applyBatch(frame, solved.finalSlices);
        if (ledgerSt != Status.OK) return ledgerSt;

        FeeAccrualEngine.FeeAccrualEngineResult feeRes = fees.accrueBatch(frame);
        if (feeRes.status != Status.OK) return feeRes.status;

        for (WireBatchRecord rec : frame.records) {
            TaxLotMatcher.TaxLot lot = new TaxLotMatcher.TaxLot();
            lot.lotId = rec.recordId;
            lot.accountId = rec.accountId;
            lot.symbolId = rec.symbolId;
            lot.qtyMilli = rec.qtyMilli;
            lot.costBasisCents = (long) rec.qtyMilli * rec.priceTick / 1000L;
            lot.acquireDateYyyymmdd = frame.header.tradeDateYyyymmdd;
            taxMatcher.openLot(lot);
        }

        benchmark.trackBatch(frame);

        st = runMarginPass(frame);
        ctx.marginOk = st == Status.OK;
        if ((frame.header.flags & WireTypes.BATCH_FLAG_MARGIN_CHECK) != 0 && !ctx.marginOk) return st;

        st = runHaircutPass(frame);
        if (st != Status.OK) return st;

        st = runCorporateActionPass(frame);
        if (st != Status.OK) return st;

        AllocationBatchSummary summary = new AllocationBatchSummary();
        summary.batchId = frame.header.deskId;
        summary.recordCount = ctx.recordCount;
        summary.totalQtyMilli = ctx.totalQtyMilli;
        summary.deskId = frame.header.deskId;
        summary.flags = frame.header.flags;
        summary.marginCleared = ctx.marginOk;
        summary.complianceCleared = ctx.complianceOk;
        AuditSpool.global().appendBatchSummary(summary);

        return Status.OK;
    }

    public Status runCompliancePass(BatchWireFrame frame) {
        RestrictionFilter.RestrictionFilterResult restricted = restrictions.filter(frame);
        if (restricted.status == Status.COMPLIANCE_REJECT) return Status.COMPLIANCE_REJECT;
        ComplianceRuleEngine.ComplianceResult cr = compliance.evaluateBatch(frame);
        return cr.passed ? Status.OK : Status.COMPLIANCE_REJECT;
    }

    public Status runMarginPass(BatchWireFrame frame) {
        MarginAggregator.MarginAggregatorResult res = margin.aggregate(frame);
        if (res.status != Status.OK && res.status != Status.MARGIN_BREACH) return res.status;
        if (res.breachCount > 0) return Status.MARGIN_BREACH;
        return Status.OK;
    }

    public Status runHaircutPass(BatchWireFrame frame) {
        HaircutCalculator.HaircutCalculatorSummary summary = haircut.computeFromBatch(frame);
        return summary.status;
    }

    public Status runCorporateActionPass(BatchWireFrame frame) {
        CorporateActionAdjuster.AdjustResult ar = corpAction.adjustBatch(frame);
        return ar.status;
    }

    public ProRataAllocator proRataAllocator() { return proRata; }
    public PositionLedger positionLedger() { return positions; }
    public ComplianceRuleEngine complianceEngine() { return compliance; }
    public MarginAggregator marginAggregator() { return margin; }
    public SettlementCalendar settlementCalendar() { return calendar; }
    public CounterpartyLimit counterpartyLimit() { return counterpartyLimit; }
    public TaxLotMatcher taxLotMatcher() { return taxMatcher; }
    public BenchmarkTracker benchmarkTracker() { return benchmark; }

    public Status runSettlementOnly(BatchWireFrame frame) {
        return calendar.computeBatch(frame).status;
    }

    public Status runRestrictionOnly(BatchWireFrame frame) {
        return restrictions.filter(frame).status;
    }

    public Status runFeeAccrualOnly(BatchWireFrame frame) {
        return fees.accrueBatch(frame).status;
    }

    public Status runLotSplitOnly(BatchWireFrame frame, LotSplitter.LotSplitPolicy policy) {
        return lotSplitter.splitBatch(frame, policy).status;
    }

    public DeskRunContext buildContext(BatchWireFrame frame) {
        DeskRunContext ctx = new DeskRunContext();
        runBatchDesks(frame, ctx);
        return ctx;
    }

    public boolean isFullyCleared(BatchWireFrame frame) {
        DeskRunContext ctx = new DeskRunContext();
        return runBatchDesks(frame, ctx) == Status.OK && ctx.marginOk && ctx.complianceOk;
    }

    public String summarizeRun(DeskRunContext ctx) {
        return "batch=" + ctx.batchId + " records=" + ctx.recordCount
                + " margin=" + ctx.marginOk + " compliance=" + ctx.complianceOk;
    }
}
