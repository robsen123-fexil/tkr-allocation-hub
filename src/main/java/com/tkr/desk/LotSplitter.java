package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.List;

/**
 * Lot splitter for round/odd lots with pro-rata and sequential policies.
 * Ported from legacy-cpp lot_splitter.cc.
 */
public final class LotSplitter {

    public enum LotSplitPolicy { PRO_RATA_BY_WEIGHT, SEQUENTIAL_FILL, ODD_LOT_FIRST, ROUND_LOT_PRESERVE }

    public static final class LotSplitRequest {
        public int parentLotId;
        public int totalQtyMilli;
        public int roundLotMilli = 1000;
        public LotSplitPolicy policy = LotSplitPolicy.PRO_RATA_BY_WEIGHT;
        public List<Integer> targetAccountIds = new ArrayList<>();
        public List<Integer> targetWeightsBp = new ArrayList<>();
    }

    public static final class LotSplitFragment {
        public int fragmentId;
        public int parentLotId;
        public int accountId;
        public int qtyMilli;
        public boolean isOddLot;
        public int sequence;
    }

    public static final class LotSplitterConfig {
        public int defaultRoundLotMilli = 1000;
        public boolean assignFragmentIds = true;
    }

    public static final class LotSplitterResult {
        public Status status = Status.OK;
        public List<LotSplitFragment> fragments = new ArrayList<>();
        public int remainderMilli;
        public int oddLotCount;
        public int roundLotCount;
    }

    private final LotSplitterConfig config;
    private int nextFragmentId = 1;

    public LotSplitter() {
        this(new LotSplitterConfig());
    }

    public LotSplitter(LotSplitterConfig config) {
        this.config = config != null ? config : new LotSplitterConfig();
    }

    public static int countRoundLots(int qtyMilli, int roundLotMilli) {
        if (roundLotMilli == 0) return 0;
        return qtyMilli / roundLotMilli;
    }

    public static int oddLotRemainder(int qtyMilli, int roundLotMilli) {
        if (roundLotMilli == 0) return qtyMilli;
        return qtyMilli % roundLotMilli;
    }

    public void assignFragmentIds(List<LotSplitFragment> fragments) {
        if (fragments == null) return;
        for (LotSplitFragment frag : fragments) frag.fragmentId = nextFragmentId++;
    }

    public LotSplitterResult splitProRata(LotSplitRequest request) {
        LotSplitterResult result = new LotSplitterResult();
        result.status = Status.OK;
        if (request.targetAccountIds.isEmpty()) {
            result.status = Status.BOUNDS_ERROR;
            return result;
        }
        int totalWeight = 0;
        for (int w : request.targetWeightsBp) totalWeight += w;
        if (totalWeight == 0) totalWeight = request.targetAccountIds.size();
        int allocated = 0;
        for (int i = 0; i < request.targetAccountIds.size(); i++) {
            int weight = i < request.targetWeightsBp.size() ? request.targetWeightsBp.get(i) : 1;
            LotSplitFragment frag = new LotSplitFragment();
            frag.parentLotId = request.parentLotId;
            frag.accountId = request.targetAccountIds.get(i);
            frag.qtyMilli = request.totalQtyMilli * weight / totalWeight;
            frag.isOddLot = frag.qtyMilli < request.roundLotMilli;
            frag.sequence = i;
            allocated += frag.qtyMilli;
            result.fragments.add(frag);
        }
        int remainder = request.totalQtyMilli - allocated;
        result.remainderMilli = remainder;
        if (remainder > 0 && !result.fragments.isEmpty()) {
            result.fragments.get(0).qtyMilli += remainder;
        }
        assignFragmentIds(result.fragments);
        for (LotSplitFragment frag : result.fragments) {
            if (frag.isOddLot) result.oddLotCount++;
            else result.roundLotCount++;
        }
        return result;
    }

    public LotSplitterResult splitSequential(LotSplitRequest request) {
        LotSplitterResult result = new LotSplitterResult();
        result.status = Status.OK;
        int remaining = request.totalQtyMilli;
        int perAccount = request.totalQtyMilli / Math.max(1, request.targetAccountIds.size());
        for (int i = 0; i < request.targetAccountIds.size(); i++) {
            LotSplitFragment frag = new LotSplitFragment();
            frag.parentLotId = request.parentLotId;
            frag.accountId = request.targetAccountIds.get(i);
            frag.sequence = i;
            frag.qtyMilli = remaining >= perAccount ? perAccount : remaining;
            remaining -= frag.qtyMilli;
            frag.isOddLot = frag.qtyMilli < request.roundLotMilli;
            result.fragments.add(frag);
        }
        if (remaining > 0 && !result.fragments.isEmpty()) {
            result.fragments.get(result.fragments.size() - 1).qtyMilli += remaining;
            result.remainderMilli = 0;
        }
        assignFragmentIds(result.fragments);
        return result;
    }

    public LotSplitterResult splitOddLotFirst(LotSplitRequest request) {
        LotSplitterResult result = new LotSplitterResult();
        int odd = oddLotRemainder(request.totalQtyMilli, request.roundLotMilli);
        int roundQty = request.totalQtyMilli - odd;
        if (odd > 0 && !request.targetAccountIds.isEmpty()) {
            LotSplitFragment oddFrag = new LotSplitFragment();
            oddFrag.parentLotId = request.parentLotId;
            oddFrag.accountId = request.targetAccountIds.get(0);
            oddFrag.qtyMilli = odd;
            oddFrag.isOddLot = true;
            oddFrag.sequence = 0;
            result.fragments.add(oddFrag);
            result.oddLotCount++;
        }
        LotSplitRequest roundReq = copyRequest(request);
        roundReq.totalQtyMilli = roundQty;
        LotSplitterResult roundResult = splitProRata(roundReq);
        for (LotSplitFragment frag : roundResult.fragments) {
            frag.sequence += 1;
            result.fragments.add(frag);
            if (frag.isOddLot) result.oddLotCount++;
            else result.roundLotCount++;
        }
        result.status = roundResult.status;
        assignFragmentIds(result.fragments);
        return result;
    }

    public LotSplitterResult splitRoundLotPreserve(LotSplitRequest request) {
        LotSplitterResult result = new LotSplitterResult();
        int roundLot = request.roundLotMilli != 0 ? request.roundLotMilli : config.defaultRoundLotMilli;
        int remaining = request.totalQtyMilli;
        for (int i = 0; i < request.targetAccountIds.size() && remaining > 0; i++) {
            LotSplitFragment frag = new LotSplitFragment();
            frag.parentLotId = request.parentLotId;
            frag.accountId = request.targetAccountIds.get(i);
            frag.sequence = i;
            int fullLots = countRoundLots(remaining, roundLot);
            if (fullLots > 0) {
                frag.qtyMilli = roundLot;
                remaining -= roundLot;
            } else {
                frag.qtyMilli = remaining;
                remaining = 0;
            }
            frag.isOddLot = frag.qtyMilli < roundLot;
            if (frag.isOddLot) result.oddLotCount++;
            else result.roundLotCount++;
            result.fragments.add(frag);
        }
        result.remainderMilli = remaining;
        result.status = Status.OK;
        assignFragmentIds(result.fragments);
        return result;
    }

    private static LotSplitRequest copyRequest(LotSplitRequest request) {
        LotSplitRequest copy = new LotSplitRequest();
        copy.parentLotId = request.parentLotId;
        copy.totalQtyMilli = request.totalQtyMilli;
        copy.roundLotMilli = request.roundLotMilli;
        copy.policy = request.policy;
        copy.targetAccountIds = new ArrayList<>(request.targetAccountIds);
        copy.targetWeightsBp = new ArrayList<>(request.targetWeightsBp);
        return copy;
    }

    public LotSplitterResult split(LotSplitRequest request) {
        return switch (request.policy) {
            case SEQUENTIAL_FILL -> splitSequential(request);
            case ODD_LOT_FIRST -> splitOddLotFirst(request);
            case ROUND_LOT_PRESERVE -> splitRoundLotPreserve(request);
            default -> splitProRata(request);
        };
    }

    public LotSplitterResult splitBatch(BatchWireFrame frame, LotSplitPolicy policy) {
        LotSplitterResult combined = new LotSplitterResult();
        combined.status = Status.OK;
        for (WireBatchRecord rec : frame.records) {
            LotSplitRequest req = new LotSplitRequest();
            req.parentLotId = rec.recordId;
            req.totalQtyMilli = rec.qtyMilli;
            req.roundLotMilli = config.defaultRoundLotMilli;
            req.policy = policy;
            req.targetAccountIds.add(rec.accountId);
            req.targetWeightsBp.add(10000);
            LotSplitterResult partial = split(req);
            if (partial.status != Status.OK) {
                combined.status = partial.status;
                return combined;
            }
            combined.fragments.addAll(partial.fragments);
            combined.oddLotCount += partial.oddLotCount;
            combined.roundLotCount += partial.roundLotCount;
        }
        return combined;
    }
}
