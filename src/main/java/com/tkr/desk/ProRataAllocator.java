package com.tkr.desk;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Pro-rata allocator using largest-remainder (Hamilton) method.
 * Example: 1000 shares at 40/30/30 weights yields 400/300/300.
 */
public final class ProRataAllocator {

    public static final class ProRataWeight {
        public int accountId;
        public int weightBp;
    }

    public static final class ProRataSlice {
        public int accountId;
        public int recordId;
        public int qtyMilli;
        public int remainderRank;
    }

    public static final class ProRataAllocatorConfig {
        public boolean distributeRemainderByLargestFraction = true;
    }

    public static final class ProRataAllocatorResult {
        public Status status = Status.OK;
        public List<ProRataSlice> slices = new ArrayList<>();
        public int totalAllocatedMilli;
        public int remainderUnits;
    }

    private final ProRataAllocatorConfig config;

    public ProRataAllocator() { this(new ProRataAllocatorConfig()); }
    public ProRataAllocator(ProRataAllocatorConfig config) {
        this.config = config != null ? config : new ProRataAllocatorConfig();
    }

    public static int computeFloorShare(int totalQty, int weightBp, int totalWeightBp) {
        if (totalWeightBp == 0 || weightBp == 0) return 0;
        long product = (long) totalQty * (long) weightBp;
        return (int) (product / totalWeightBp);
    }

    public static int computeFractionalRemainder(int totalQty, int weightBp, int totalWeightBp) {
        if (totalWeightBp == 0 || weightBp == 0) return 0;
        long product = (long) totalQty * (long) weightBp;
        return (int) (product % totalWeightBp);
    }

    private static int sumWeightBp(List<ProRataWeight> weights) {
        int total = 0;
        for (ProRataWeight w : weights) total += w.weightBp;
        return total;
    }

    public ProRataAllocatorResult allocateWithWeights(int totalQtyMilli, List<ProRataWeight> weights,
                                                      List<WireBatchRecord> records) {
        ProRataAllocatorResult result = new ProRataAllocatorResult();
        Status validated = validateWeights(weights);
        if (validated != Status.OK) { result.status = validated; return result; }
        int totalWeightBp = sumWeightBp(weights);
        int allocated = 0;
        for (int i = 0; i < weights.size(); i++) {
            ProRataWeight w = weights.get(i);
            ProRataSlice slice = new ProRataSlice();
            slice.accountId = w.accountId;
            slice.recordId = (records != null && i < records.size()) ? records.get(i).recordId : 0;
            slice.qtyMilli = computeFloorShare(totalQtyMilli, w.weightBp, totalWeightBp);
            slice.remainderRank = 0;
            allocated += slice.qtyMilli;
            result.slices.add(slice);
        }
        sortByFractionalRemainder(result.slices, weights, totalQtyMilli, totalWeightBp);
        int remainder = totalQtyMilli - allocated;
        result.remainderUnits = remainder;
        if (config.distributeRemainderByLargestFraction && remainder > 0) {
            List<ProRataSlice> ranked = new ArrayList<>(result.slices);
            ranked.sort(Comparator.comparingInt((ProRataSlice s) -> s.remainderRank)
                    .thenComparingInt(s -> s.accountId));
            for (int r = 0; r < remainder && r < ranked.size(); r++) {
                int targetAccount = ranked.get(r).accountId;
                for (ProRataSlice slice : result.slices) {
                    if (slice.accountId == targetAccount) { slice.qtyMilli += 1; break; }
                }
            }
            result.totalAllocatedMilli = totalQtyMilli;
        } else {
            result.totalAllocatedMilli = allocated;
        }
        result.status = Status.OK;
        return result;
    }

    public ProRataAllocatorResult allocate(BatchWireFrame frame) {
        ProRataAllocatorResult result = new ProRataAllocatorResult();
        if (frame == null || frame.records.isEmpty()) { result.status = Status.BOUNDS_ERROR; return result; }
        int totalQty = 0;
        for (WireBatchRecord rec : frame.records) totalQty += rec.qtyMilli;
        List<ProRataWeight> weights;
        if ((frame.header.flags & WireTypes.BATCH_FLAG_PRO_RATA) != 0) {
            weights = extractWeightsFromFrame(frame);
        } else {
            weights = equalWeights(frame.records.size(), frame.records);
        }
        result = allocateWithWeights(totalQty, weights, frame.records);
        if (result.status == Status.OK && !verifyTotalConservation(result.slices, totalQty)) {
            result.status = Status.BOUNDS_ERROR;
        }
        return result;
    }

    private Status validateWeights(List<ProRataWeight> weights) {
        if (weights == null || weights.isEmpty()) return Status.BOUNDS_ERROR;
        if (sumWeightBp(weights) == 0) return Status.BOUNDS_ERROR;
        for (ProRataWeight w : weights) if (w.weightBp == 0) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    private List<ProRataWeight> extractWeightsFromFrame(BatchWireFrame frame) {
        List<ProRataWeight> weights = new ArrayList<>();
        for (WireBatchRecord rec : frame.records) {
            ProRataWeight w = new ProRataWeight();
            w.accountId = rec.accountId;
            w.weightBp = rec.qtyMilli;
            weights.add(w);
        }
        return weights;
    }

    private List<ProRataWeight> equalWeights(int count, List<WireBatchRecord> records) {
        List<ProRataWeight> weights = new ArrayList<>();
        int equalBp = 10000 / count;
        int remainderBp = 10000 - equalBp * count;
        for (int i = 0; i < count; i++) {
            ProRataWeight w = new ProRataWeight();
            w.accountId = records.get(i).accountId;
            w.weightBp = equalBp + (i < remainderBp ? 1 : 0);
            weights.add(w);
        }
        return weights;
    }

    private void sortByFractionalRemainder(List<ProRataSlice> slices, List<ProRataWeight> weights,
                                           int totalQtyMilli, int totalWeightBp) {
        if (!config.distributeRemainderByLargestFraction) return;
        record FractionalRank(int accountId, int remainder, int rank) {}
        List<FractionalRank> ranks = new ArrayList<>();
        for (ProRataSlice slice : slices) {
            int remainder = 0;
            for (ProRataWeight w : weights) {
                if (w.accountId == slice.accountId) {
                    remainder = computeFractionalRemainder(totalQtyMilli, w.weightBp, totalWeightBp);
                    break;
                }
            }
            ranks.add(new FractionalRank(slice.accountId, remainder, 0));
        }
        ranks.sort(Comparator.comparingInt((FractionalRank r) -> r.remainder).reversed()
                .thenComparingInt(r -> r.accountId));
        for (int i = 0; i < ranks.size(); i++) {
            FractionalRank fr = ranks.get(i);
            for (ProRataSlice slice : slices) {
                if (slice.accountId == fr.accountId) slice.remainderRank = i;
            }
        }
    }

    private boolean verifyTotalConservation(List<ProRataSlice> slices, int expectedTotal) {
        int sum = 0;
        for (ProRataSlice s : slices) sum += s.qtyMilli;
        return sum == expectedTotal;
    }
// --- expanded helpers ---
    public List<ProRataSlice> simulateWaterfall(int totalQtyMilli, List<ProRataWeight> weights) {
        List<ProRataSlice> stages = new ArrayList<>();
        int remaining = totalQtyMilli;
        int totalWeight = sumWeightBp(weights);
        for (int pass = 0; pass < weights.size() && remaining > 0; pass++) {
            ProRataWeight w = weights.get(pass);
            int share = computeFloorShare(remaining, w.weightBp, totalWeight);
            ProRataSlice slice = new ProRataSlice();
            slice.accountId = w.accountId;
            slice.qtyMilli = share;
            stages.add(slice);
            remaining -= share;
        }
        if (remaining > 0 && !stages.isEmpty()) {
            stages.get(stages.size() - 1).qtyMilli += remaining;
        }
        return stages;
    }

    public int[] computeHamiltonApportionment(int seats, List<ProRataWeight> weights) {
        int totalWeight = sumWeightBp(weights);
        int[] quotas = new int[weights.size()];
        int[] assigned = new int[weights.size()];
        int allocated = 0;
        for (int i = 0; i < weights.size(); i++) {
            quotas[i] = computeFloorShare(seats, weights.get(i).weightBp, totalWeight);
            assigned[i] = quotas[i];
            allocated += quotas[i];
        }
        int remainder = seats - allocated;
        Integer[] order = new Integer[weights.size()];
        for (int i = 0; i < order.length; i++) order[i] = i;
        java.util.Arrays.sort(order, (a, b) -> {
            int ra = computeFractionalRemainder(seats, weights.get(a).weightBp, totalWeight);
            int rb = computeFractionalRemainder(seats, weights.get(b).weightBp, totalWeight);
            if (ra != rb) return Integer.compare(rb, ra);
            return Integer.compare(weights.get(a).accountId, weights.get(b).accountId);
        });
        for (int i = 0; i < remainder; i++) assigned[order[i]]++;
        return assigned;
    }

    public String formatAllocationTable(ProRataAllocatorResult result, List<ProRataWeight> weights) {
        StringBuilder sb = new StringBuilder();
        sb.append("account,qty,weight_bp\n");
        for (ProRataSlice slice : result.slices) {
            int weight = 0;
            for (ProRataWeight w : weights) if (w.accountId == slice.accountId) weight = w.weightBp;
            sb.append(slice.accountId).append(',').append(slice.qtyMilli).append(',').append(weight).append('\n');
        }
        return sb.toString();
    }
}
