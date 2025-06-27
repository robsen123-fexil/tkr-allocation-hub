package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.List;

/**
 * Corporate action adjuster for splits, dividends, mergers, spin-offs.
 * Ported from legacy-cpp corporate_action_adjuster.cc.
 */
public final class CorporateActionAdjuster {

    public enum CorporateActionKind {
        STOCK_SPLIT, REVERSE_SPLIT, CASH_DIVIDEND, STOCK_DIVIDEND, MERGER, SPIN_OFF
    }

    public static final class CorporateAction {
        public int actionId;
        public CorporateActionKind kind;
        public int symbolId;
        public int newSymbolId;
        public long ratioNumerator;
        public long ratioDenominator;
        public long cashAmountCents;
    }

    public static final class AdjustedPosition {
        public int accountId;
        public int symbolId;
        public long oldQtyMilli;
        public long newQtyMilli;
        public long cashAdjustmentCents;
        public int actionId;
    }

    public static final class CorporateActionAdjusterConfig {
        public int deskId;

        public CorporateActionAdjusterConfig(int deskId) {
            this.deskId = deskId;
        }
    }

    public static final class CorporateActionAdjusterResult {
        public Status status = Status.OK;
        public List<AdjustedPosition> adjustments = new ArrayList<>();
        public long totalCashCents;
        public int actionsApplied;
    }

    /** Legacy wrapper. */
    public static final class AdjustResult {
        public Status status = Status.OK;
        public List<AdjustedPosition> adjustments = new ArrayList<>();
    }

    private final CorporateActionAdjusterConfig config;
    private final List<CorporateAction> pendingActions = new ArrayList<>();

    public CorporateActionAdjuster() {
        this(new CorporateActionAdjusterConfig(0));
    }

    public CorporateActionAdjuster(CorporateActionAdjusterConfig config) {
        this.config = config != null ? config : new CorporateActionAdjusterConfig(0);
    }

    public void registerAction(CorporateAction action) {
        if (action != null) pendingActions.add(action);
    }

    public void clearActions() {
        pendingActions.clear();
    }

    public static long adjustQtyForSplit(long qtyMilli, long numerator, long denominator) {
        if (denominator == 0) return qtyMilli;
        return qtyMilli * numerator / denominator;
    }

    public static long computeCashDividend(long qtyMilli, long cashPerShareCents) {
        return qtyMilli * cashPerShareCents / 1000L;
    }

    public AdjustedPosition applyStockSplit(CorporateAction action, PositionLedger.PositionEntry pos) {
        AdjustedPosition adj = new AdjustedPosition();
        adj.accountId = pos.accountId;
        adj.symbolId = pos.symbolId;
        adj.oldQtyMilli = pos.qtyMilli;
        adj.newQtyMilli = adjustQtyForSplit(pos.qtyMilli, action.ratioNumerator, action.ratioDenominator);
        adj.cashAdjustmentCents = 0;
        adj.actionId = action.actionId;
        return adj;
    }

    public AdjustedPosition applyReverseSplit(CorporateAction action, PositionLedger.PositionEntry pos) {
        return applyStockSplit(action, pos);
    }

    public AdjustedPosition applyCashDividend(CorporateAction action, PositionLedger.PositionEntry pos) {
        AdjustedPosition adj = new AdjustedPosition();
        adj.accountId = pos.accountId;
        adj.symbolId = pos.symbolId;
        adj.oldQtyMilli = pos.qtyMilli;
        adj.newQtyMilli = pos.qtyMilli;
        adj.cashAdjustmentCents = computeCashDividend(pos.qtyMilli, action.cashAmountCents);
        adj.actionId = action.actionId;
        return adj;
    }

    public AdjustedPosition applyStockDividend(CorporateAction action, PositionLedger.PositionEntry pos) {
        AdjustedPosition adj = applyStockSplit(action, pos);
        adj.cashAdjustmentCents = 0;
        return adj;
    }

    public AdjustedPosition applyMerger(CorporateAction action, PositionLedger.PositionEntry pos) {
        AdjustedPosition adj = new AdjustedPosition();
        adj.accountId = pos.accountId;
        adj.symbolId = action.newSymbolId;
        adj.oldQtyMilli = pos.qtyMilli;
        adj.newQtyMilli = adjustQtyForSplit(pos.qtyMilli, action.ratioNumerator, action.ratioDenominator);
        adj.cashAdjustmentCents = action.cashAmountCents;
        adj.actionId = action.actionId;
        return adj;
    }

    public AdjustedPosition applySpinOff(CorporateAction action, PositionLedger.PositionEntry pos) {
        AdjustedPosition adj = new AdjustedPosition();
        adj.accountId = pos.accountId;
        adj.symbolId = action.newSymbolId;
        adj.oldQtyMilli = pos.qtyMilli;
        adj.newQtyMilli = adjustQtyForSplit(pos.qtyMilli, action.ratioNumerator, action.ratioDenominator);
        adj.cashAdjustmentCents = 0;
        adj.actionId = action.actionId;
        return adj;
    }

    public CorporateActionAdjusterResult applyActions(List<PositionLedger.PositionEntry> positions) {
        CorporateActionAdjusterResult result = new CorporateActionAdjusterResult();
        result.status = Status.OK;
        for (PositionLedger.PositionEntry pos : positions) {
            for (CorporateAction action : pendingActions) {
                if (action.symbolId != pos.symbolId) continue;
                AdjustedPosition adj = switch (action.kind) {
                    case STOCK_SPLIT -> applyStockSplit(action, pos);
                    case REVERSE_SPLIT -> applyReverseSplit(action, pos);
                    case CASH_DIVIDEND -> applyCashDividend(action, pos);
                    case STOCK_DIVIDEND -> applyStockDividend(action, pos);
                    case MERGER -> applyMerger(action, pos);
                    case SPIN_OFF -> applySpinOff(action, pos);
                };
                result.adjustments.add(adj);
                result.totalCashCents += adj.cashAdjustmentCents;
                result.actionsApplied++;
            }
        }
        return result;
    }

    public CorporateActionAdjusterResult applyToBatch(BatchWireFrame frame) {
        List<PositionLedger.PositionEntry> positions = new ArrayList<>();
        for (WireBatchRecord rec : frame.records) {
            PositionLedger.PositionEntry pos = new PositionLedger.PositionEntry();
            pos.accountId = rec.accountId;
            pos.symbolId = rec.symbolId;
            pos.qtyMilli = rec.qtyMilli;
            positions.add(pos);
        }
        return applyActions(positions);
    }

    /** Legacy API. */
    public AdjustResult adjustBatch(BatchWireFrame frame) {
        CorporateActionAdjusterResult engine = applyToBatch(frame);
        AdjustResult legacy = new AdjustResult();
        legacy.status = engine.status;
        legacy.adjustments = engine.adjustments;
        return legacy;
    }

    public List<CorporateAction> pendingActions() {
        return new ArrayList<>(pendingActions);
    }
}
