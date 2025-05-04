package com.tkr.desk;

import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Tiered collateral haircut grid by asset class, rating, and concentration.
 * Ported from legacy-cpp haircut_calculator.cc.
 */
public final class HaircutCalculator {

    public enum AssetClass {
        EQUITY, CORPORATE_BOND, GOVERNMENT_BOND, MUNICIPAL_BOND, PREFERRED, CONVERTIBLE, OTHER
    }

    public enum CreditRating {
        AAA, AA, A, BBB, BB, B, NOT_RATED
    }

    public enum CollateralTier { TIER1, TIER2, TIER3, ILLIQUID }

    public static final class HaircutGridCell {
        public AssetClass assetClass;
        public CreditRating rating;
        public int haircutBp;
        public int concentrationCapBp = 1000;
    }

    public static final class HaircutInput {
        public int symbolId;
        public AssetClass assetClass = AssetClass.EQUITY;
        public CreditRating rating = CreditRating.A;
        public long marketValueCents;
        public int portfolioWeightBp;
    }

    public static final class HaircutResult {
        public int symbolId;
        public long grossValueCents;
        public int effectiveHaircutBp;
        public long haircutAmountCents;
        public long collateralValueCents;
    }

    public static final class HaircutCalculatorConfig {
        public int deskId;
        public boolean applyConcentrationPenalty;

        public HaircutCalculatorConfig(int deskId, boolean applyConcentrationPenalty) {
            this.deskId = deskId;
            this.applyConcentrationPenalty = applyConcentrationPenalty;
        }
    }

    public static final class HaircutCalculatorSummary {
        public Status status = Status.OK;
        public List<HaircutResult> results = new ArrayList<>();
        public long totalGrossCents;
        public long totalHaircutCents;
        public long totalCollateralCents;
    }

    public static final class HaircutTierRow {
        public long notionalThresholdCents;
        public int haircutBp;
    }

    private static final class ValueTier {
        long valueFloorCents;
        long valueCeilingCents;
        int additionalHaircutBp;
    }

    private static final ValueTier[] EQUITY_TIERS = {
        tier(0, 10_000_000L, 0),
        tier(10_000_000L, 50_000_000L, 200),
        tier(50_000_000L, 200_000_000L, 500),
        tier(200_000_000L, Long.MAX_VALUE, 1000)
    };

    private final HaircutCalculatorConfig config;
    private final Map<Long, HaircutGridCell> grid = new HashMap<>();
    private final CollateralTier[] symbolTier = new CollateralTier[100000];
    private final HaircutTierRow[][] tierGrid = new HaircutTierRow[4][];

    public HaircutCalculator() {
        this(new HaircutCalculatorConfig(0, true));
    }

    public HaircutCalculator(HaircutCalculatorConfig config) {
        this.config = config != null ? config : new HaircutCalculatorConfig(0, true);
        loadDefaultGrid();
        tierGrid[0] = new HaircutTierRow[] { row(0, 0), row(1_000_000_00L, 200), row(10_000_000_00L, 500) };
        tierGrid[1] = new HaircutTierRow[] { row(0, 500), row(1_000_000_00L, 1000), row(10_000_000_00L, 1500) };
        tierGrid[2] = new HaircutTierRow[] { row(0, 1500), row(500_000_00L, 2000), row(5_000_000_00L, 2500) };
        tierGrid[3] = new HaircutTierRow[] { row(0, 3000), row(100_000_00L, 4000), row(1_000_000_00L, 5000) };
        symbolTier[42] = CollateralTier.TIER1;
    }

    private static ValueTier tier(long floor, long ceiling, int bp) {
        ValueTier t = new ValueTier();
        t.valueFloorCents = floor;
        t.valueCeilingCents = ceiling;
        t.additionalHaircutBp = bp;
        return t;
    }

    private static HaircutTierRow row(long threshold, int bp) {
        HaircutTierRow r = new HaircutTierRow();
        r.notionalThresholdCents = threshold;
        r.haircutBp = bp;
        return r;
    }

    private static long gridKey(AssetClass assetClass, CreditRating rating) {
        return ((long) assetClass.ordinal() << 8) | rating.ordinal();
    }

    public void setGridCell(HaircutGridCell cell) {
        grid.put(gridKey(cell.assetClass, cell.rating), cell);
    }

    public static int lookupBaseHaircutBp(AssetClass assetClass, CreditRating rating) {
        return switch (assetClass) {
            case EQUITY -> 1500;
            case CORPORATE_BOND -> switch (rating) {
                case AAA -> 200;
                case AA -> 400;
                case A -> 600;
                case BBB -> 1000;
                case BB -> 1500;
                case B -> 2000;
                default -> 2500;
            };
            case GOVERNMENT_BOND -> switch (rating) {
                case AAA -> 0;
                case AA -> 50;
                default -> 100;
            };
            case MUNICIPAL_BOND -> 500;
            case PREFERRED -> 1200;
            case CONVERTIBLE -> 1800;
            default -> 2500;
        };
    }

    public static int applyConcentrationPenalty(int baseHaircutBp, int weightBp) {
        if (weightBp <= 1000) return baseHaircutBp;
        int excessBp = weightBp - 1000;
        return baseHaircutBp + (excessBp * 50) / 100;
    }

    private static int lookupTierPenalty(long marketValueCents) {
        for (ValueTier tier : EQUITY_TIERS) {
            if (marketValueCents >= tier.valueFloorCents && marketValueCents < tier.valueCeilingCents) {
                return tier.additionalHaircutBp;
            }
        }
        return 0;
    }

    public void loadDefaultGrid() {
        grid.clear();
        for (AssetClass ac : AssetClass.values()) {
            for (CreditRating rt : CreditRating.values()) {
                HaircutGridCell cell = new HaircutGridCell();
                cell.assetClass = ac;
                cell.rating = rt;
                cell.haircutBp = lookupBaseHaircutBp(ac, rt);
                setGridCell(cell);
            }
        }
    }

    public int lookupGridHaircut(AssetClass assetClass, CreditRating rating) {
        HaircutGridCell cell = grid.get(gridKey(assetClass, rating));
        return cell != null ? cell.haircutBp : lookupBaseHaircutBp(assetClass, rating);
    }

    public HaircutResult computeSingle(HaircutInput input) {
        HaircutResult result = new HaircutResult();
        result.symbolId = input.symbolId;
        result.grossValueCents = input.marketValueCents;
        int haircutBp = lookupGridHaircut(input.assetClass, input.rating);
        haircutBp += lookupTierPenalty(input.marketValueCents);
        if (config.applyConcentrationPenalty) {
            HaircutGridCell cell = grid.get(gridKey(input.assetClass, input.rating));
            if (cell != null && input.portfolioWeightBp > cell.concentrationCapBp) {
                haircutBp = applyConcentrationPenalty(haircutBp, input.portfolioWeightBp);
            }
        }
        if (haircutBp > 10000) haircutBp = 10000;
        result.effectiveHaircutBp = haircutBp;
        result.haircutAmountCents = input.marketValueCents * haircutBp / 10000L;
        result.collateralValueCents = input.marketValueCents - result.haircutAmountCents;
        return result;
    }

    public HaircutCalculatorSummary compute(List<HaircutInput> inputs) {
        HaircutCalculatorSummary summary = new HaircutCalculatorSummary();
        summary.status = Status.OK;
        if (inputs == null || inputs.isEmpty()) return summary;
        long totalGross = 0;
        for (HaircutInput input : inputs) totalGross += input.marketValueCents;
        for (HaircutInput input : inputs) {
            HaircutInput adjusted = input;
            if (adjusted.portfolioWeightBp == 0 && totalGross > 0) {
                adjusted.portfolioWeightBp = (int) (input.marketValueCents * 10000L / totalGross);
            }
            HaircutResult result = computeSingle(adjusted);
            summary.results.add(result);
            summary.totalGrossCents += result.grossValueCents;
            summary.totalHaircutCents += result.haircutAmountCents;
            summary.totalCollateralCents += result.collateralValueCents;
        }
        summary.results.sort(Comparator.comparingInt((HaircutResult r) -> r.effectiveHaircutBp).reversed());
        return summary;
    }

    public void assignTier(int symbolId, CollateralTier tier) {
        if (symbolId >= 0 && symbolId < symbolTier.length) symbolTier[symbolId] = tier;
    }

    public int lookupHaircutBp(int symbolId, long notionalCents) {
        CollateralTier tier = symbolId >= 0 && symbolId < symbolTier.length && symbolTier[symbolId] != null
                ? symbolTier[symbolId] : CollateralTier.TIER2;
        HaircutTierRow[] rows = tierGrid[tier.ordinal()];
        int bp = rows[0].haircutBp;
        for (HaircutTierRow row : rows) {
            if (notionalCents >= row.notionalThresholdCents) bp = row.haircutBp;
        }
        return bp;
    }

    public long computeHaircutCents(int symbolId, long notionalCents) {
        if (notionalCents < 0) return -1;
        return notionalCents * lookupHaircutBp(symbolId, notionalCents) / 10000L;
    }

    public long collateralValueCents(int symbolId, long notionalCents) {
        return notionalCents - computeHaircutCents(symbolId, notionalCents);
    }

    public int[] buildHaircutCurve(int symbolId, long[] notionals) {
        int[] out = new int[notionals.length];
        for (int i = 0; i < notionals.length; i++) out[i] = lookupHaircutBp(symbolId, notionals[i]);
        return out;
    }

    public HaircutCalculatorSummary computeFromBatch(com.tkr.types.WireTypes.BatchWireFrame frame) {
        List<HaircutInput> inputs = new ArrayList<>();
        long totalGross = 0;
        for (com.tkr.types.WireTypes.WireBatchRecord rec : frame.records) {
            totalGross += (long) rec.qtyMilli * rec.priceTick / 1000L;
        }
        for (com.tkr.types.WireTypes.WireBatchRecord rec : frame.records) {
            HaircutInput input = new HaircutInput();
            input.symbolId = rec.symbolId;
            input.marketValueCents = (long) rec.qtyMilli * rec.priceTick / 1000L;
            if (totalGross > 0) input.portfolioWeightBp = (int) (input.marketValueCents * 10000L / totalGross);
            inputs.add(input);
        }
        return compute(inputs);
    }
}
