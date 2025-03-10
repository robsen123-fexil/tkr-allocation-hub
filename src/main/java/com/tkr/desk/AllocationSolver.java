package com.tkr.desk;

import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Min/max constraint solver adjusting pro-rata slices via deficit redistribution. */
public final class AllocationSolver {

    public static final class AllocationBound {
        public int accountId;
        public int minQtyMilli;
        public int maxQtyMilli;
    }

    public static final class AllocationSolverConfig {
        public boolean iterativeRedistribute = true;
        public int maxIterations = 100;
    }

    public static final class AllocationSolverResult {
        public Status status = Status.OK;
        public List<ProRataAllocator.ProRataSlice> finalSlices = new ArrayList<>();
        public boolean converged;
        public int iterations;
    }

    private final AllocationSolverConfig config;
    private final List<AllocationBound> customBounds = new ArrayList<>();

    public AllocationSolver() { this(new AllocationSolverConfig()); }
    public AllocationSolver(AllocationSolverConfig config) {
        this.config = config != null ? config : new AllocationSolverConfig();
    }

    public void setBound(int accountId, int minQty, int maxQty) {
        for (AllocationBound b : customBounds) {
            if (b.accountId == accountId) { b.minQtyMilli = minQty; b.maxQtyMilli = maxQty; return; }
        }
        AllocationBound bound = new AllocationBound();
        bound.accountId = accountId;
        bound.minQtyMilli = minQty;
        bound.maxQtyMilli = maxQty;
        customBounds.add(bound);
    }

    public void clearBounds() { customBounds.clear(); }

    public AllocationSolverResult solve(com.tkr.types.WireTypes.BatchWireFrame frame,
                                        List<ProRataAllocator.ProRataSlice> slices) {
        int targetTotal = 0;
        if (frame != null) {
            for (com.tkr.types.WireTypes.WireBatchRecord rec : frame.records) targetTotal += rec.qtyMilli;
        }
        return solveWithBounds(slices, null, targetTotal);
    }

    public AllocationSolverResult solveWithBounds(List<ProRataAllocator.ProRataSlice> slices,
                                                  List<AllocationBound> bounds, int targetTotal) {
        AllocationSolverResult result = new AllocationSolverResult();
        List<AllocationBound> allBounds = new ArrayList<>(customBounds);
        if (bounds != null) allBounds.addAll(bounds);
        List<ProRataAllocator.ProRataSlice> working = deepCopy(slices);
        result.iterations = 0;
        boolean stable = false;
        while (!stable && result.iterations < config.maxIterations) {
            stable = true;
            result.iterations++;
            List<Violation> violations = detectViolations(working, allBounds);
            if (violations.isEmpty()) break;
            for (Violation v : violations) {
                if (v.surplus > 0) {
                    clampSlice(findSlice(working, v.accountId), v.maxQty);
                    Status st = redistributeSurplus(working, v.surplus, v.accountId);
                    if (st != Status.OK) { result.status = st; return result; }
                    stable = false;
                } else if (v.deficit > 0) {
                    clampSlice(findSlice(working, v.accountId), v.minQty);
                    Status st = redistributeDeficit(working, v.deficit, v.accountId);
                    if (st != Status.OK) { result.status = st; return result; }
                    stable = false;
                }
            }
        }
        result.converged = stable || detectViolations(working, allBounds).isEmpty();
        result.finalSlices = working;
        int sum = sumSliceQty(working);
        if (sum != targetTotal) result.status = Status.BOUNDS_ERROR;
        return result;
    }

    private static final class Violation {
        int accountId, currentQty, minQty, maxQty, deficit, surplus;
    }

    private List<Violation> detectViolations(List<ProRataAllocator.ProRataSlice> slices, List<AllocationBound> bounds) {
        List<Violation> out = new ArrayList<>();
        for (ProRataAllocator.ProRataSlice slice : slices) {
            AllocationBound bound = findBound(bounds, slice.accountId);
            if (bound == null) continue;
            Violation v = new Violation();
            v.accountId = slice.accountId;
            v.currentQty = slice.qtyMilli;
            v.minQty = bound.minQtyMilli;
            v.maxQty = bound.maxQtyMilli;
            if (slice.qtyMilli < bound.minQtyMilli) v.deficit = bound.minQtyMilli - slice.qtyMilli;
            if (slice.qtyMilli > bound.maxQtyMilli) v.surplus = slice.qtyMilli - bound.maxQtyMilli;
            if (v.deficit > 0 || v.surplus > 0) out.add(v);
        }
        return out;
    }

    private AllocationBound findBound(List<AllocationBound> bounds, int accountId) {
        for (AllocationBound b : bounds) if (b.accountId == accountId) return b;
        return null;
    }

    private ProRataAllocator.ProRataSlice findSlice(List<ProRataAllocator.ProRataSlice> slices, int accountId) {
        for (ProRataAllocator.ProRataSlice s : slices) if (s.accountId == accountId) return s;
        return null;
    }

    private void clampSlice(ProRataAllocator.ProRataSlice slice, int limit) {
        if (slice == null) return;
        slice.qtyMilli = limit;
    }

    private Status redistributeDeficit(List<ProRataAllocator.ProRataSlice> slices, int deficit, int excludeAccount) {
        if (deficit <= 0) return Status.OK;
        slices.sort(Comparator.comparingInt(s -> s.qtyMilli));
        int remaining = deficit;
        for (int i = slices.size() - 1; i >= 0 && remaining > 0; i--) {
            ProRataAllocator.ProRataSlice s = slices.get(i);
            if (s.accountId == excludeAccount || s.qtyMilli <= 0) continue;
            s.qtyMilli -= 1;
            findSlice(slices, excludeAccount).qtyMilli += 1;
            remaining--;
        }
        return remaining == 0 ? Status.OK : Status.BOUNDS_ERROR;
    }

    private Status redistributeSurplus(List<ProRataAllocator.ProRataSlice> slices, int surplus, int excludeAccount) {
        if (surplus <= 0) return Status.OK;
        int remaining = surplus;
        for (ProRataAllocator.ProRataSlice s : slices) {
            if (s.accountId == excludeAccount || remaining <= 0) continue;
            s.qtyMilli += 1;
            findSlice(slices, excludeAccount).qtyMilli -= 1;
            remaining--;
        }
        return remaining == 0 ? Status.OK : Status.BOUNDS_ERROR;
    }

    private int sumSliceQty(List<ProRataAllocator.ProRataSlice> slices) {
        int total = 0;
        for (ProRataAllocator.ProRataSlice s : slices) total += s.qtyMilli;
        return total;
    }

    private List<ProRataAllocator.ProRataSlice> deepCopy(List<ProRataAllocator.ProRataSlice> slices) {
        List<ProRataAllocator.ProRataSlice> out = new ArrayList<>();
        for (ProRataAllocator.ProRataSlice s : slices) {
            ProRataAllocator.ProRataSlice c = new ProRataAllocator.ProRataSlice();
            c.accountId = s.accountId;
            c.recordId = s.recordId;
            c.qtyMilli = s.qtyMilli;
            c.remainderRank = s.remainderRank;
            out.add(c);
        }
        return out;
    }
// --- expanded helpers ---
    public int computeSlack(List<ProRataAllocator.ProRataSlice> slices, List<AllocationBound> bounds) {
        int slack = 0;
        for (ProRataAllocator.ProRataSlice slice : slices) {
            AllocationBound b = findBound(bounds, slice.accountId);
            if (b != null) slack += Math.max(0, b.maxQtyMilli - slice.qtyMilli);
        }
        return slack;
    }

    public boolean isFeasible(List<AllocationBound> bounds, int targetTotal) {
        int minSum = 0, maxSum = 0;
        for (AllocationBound b : bounds) {
            minSum += b.minQtyMilli;
            maxSum += b.maxQtyMilli;
        }
        return targetTotal >= minSum && targetTotal <= maxSum;
    }
}
