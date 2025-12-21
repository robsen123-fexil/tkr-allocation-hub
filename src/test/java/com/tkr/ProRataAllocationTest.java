package com.tkr;

import com.tkr.desk.AllocationSolver;
import com.tkr.desk.ProRataAllocator;
import com.tkr.types.WireTypes.Status;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

/** Integration tests for pro-rata allocation and bound solver. */
class ProRataAllocationTest {

    @Test
    void proRata1000Shares40_30_30() {
        List<ProRataAllocator.ProRataWeight> weights = new ArrayList<>();
        List<com.tkr.types.WireTypes.WireBatchRecord> records = new ArrayList<>();
        int[] accounts = { 101, 102, 103 };
        int[] weightVals = { 4000, 3000, 3000 };
        for (int i = 0; i < 3; i++) {
            ProRataAllocator.ProRataWeight w = new ProRataAllocator.ProRataWeight();
            w.accountId = accounts[i];
            w.weightBp = weightVals[i];
            weights.add(w);
            com.tkr.types.WireTypes.WireBatchRecord rec = new com.tkr.types.WireTypes.WireBatchRecord();
            rec.recordId = i + 1;
            rec.accountId = accounts[i];
            rec.qtyMilli = weightVals[i];
            records.add(rec);
        }
        ProRataAllocator allocator = new ProRataAllocator();
        ProRataAllocator.ProRataAllocatorResult result =
                allocator.allocateWithWeights(1000, weights, records);
        assertEquals(Status.OK, result.status);
        assertEquals(3, result.slices.size());
        int[] expected = { 400, 300, 300 };
        for (int i = 0; i < 3; i++) {
            final int account = accounts[i];
            ProRataAllocator.ProRataSlice slice = result.slices.stream()
                    .filter(s -> s.accountId == account).findFirst().orElseThrow();
            assertEquals(expected[i], slice.qtyMilli, "account " + account);
        }
        assertEquals(1000, result.totalAllocatedMilli);
    }

    @Test
    void allocationSolverRespectsMaxBound() {
        List<ProRataAllocator.ProRataSlice> slices = new ArrayList<>();
        ProRataAllocator.ProRataSlice s1 = new ProRataAllocator.ProRataSlice();
        s1.accountId = 101;
        s1.qtyMilli = 600;
        slices.add(s1);
        ProRataAllocator.ProRataSlice s2 = new ProRataAllocator.ProRataSlice();
        s2.accountId = 102;
        s2.qtyMilli = 400;
        slices.add(s2);
        List<AllocationSolver.AllocationBound> bounds = new ArrayList<>();
        AllocationSolver.AllocationBound b1 = new AllocationSolver.AllocationBound();
        b1.accountId = 101;
        b1.maxQtyMilli = 500;
        bounds.add(b1);
        AllocationSolver solver = new AllocationSolver();
        AllocationSolver.AllocationSolverResult result = solver.solveWithBounds(slices, bounds, 1000);
        for (ProRataAllocator.ProRataSlice slice : result.finalSlices) {
            if (slice.accountId == 101) {
                assertTrue(slice.qtyMilli <= 500);
            }
        }
    }
}
