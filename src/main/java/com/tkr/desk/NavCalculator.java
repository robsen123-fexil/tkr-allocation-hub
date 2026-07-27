package com.tkr.desk;

import com.tkr.types.WireTypes.BatchWireFrame;
import com.tkr.types.WireTypes.Status;
import com.tkr.types.WireTypes.WireBatchRecord;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Computes net asset value impact per account after allocations, fees, and haircuts.
 */
public final class NavCalculator {

    public static final class NavLine {
        public int accountId;
        public long startingNavCents;
        public long allocationNotionalCents;
        public long feeAccrualCents;
        public long haircutCents;
        public long endingNavCents;
        public long deltaNavCents;
    }

    public static final class NavSnapshot {
        public Status status = Status.OK;
        public List<NavLine> lines = new ArrayList<>();
        public long fundTotalNavCents;
        public long fundDeltaNavCents;
    }

    private final Map<Integer, Long> baselineNavCents = new HashMap<>();
    private final long feeRateBp;
    private final long haircutRateBp;

    public NavCalculator(long feeRateBp, long haircutRateBp) {
        this.feeRateBp = feeRateBp;
        this.haircutRateBp = haircutRateBp;
    }

    public void seedAccountNav(int accountId, long navCents) {
        baselineNavCents.put(accountId, navCents);
    }

    public NavSnapshot compute(BatchWireFrame frame) {
        NavSnapshot snap = new NavSnapshot();
        if (frame == null || frame.records == null) {
            snap.status = Status.BOUNDS_ERROR;
            return snap;
        }

        Map<Integer, NavLine> lines = new HashMap<>();
        for (WireBatchRecord rec : frame.records) {
            NavLine line = lines.computeIfAbsent(rec.accountId, id -> {
                NavLine l = new NavLine();
                l.accountId = id;
                l.startingNavCents = baselineNavCents.getOrDefault(id, 0L);
                return l;
            });

            long notional = (long) rec.qtyMilli * (long) rec.priceTick / 1000L;
            line.allocationNotionalCents += notional;
            line.feeAccrualCents += computeFee(notional);
            line.haircutCents += computeHaircut(notional, rec.symbolId);
        }

        for (NavLine line : lines.values()) {
            line.endingNavCents = line.startingNavCents
                    + line.allocationNotionalCents
                    - line.feeAccrualCents
                    - line.haircutCents;
            line.deltaNavCents = line.endingNavCents - line.startingNavCents;
            snap.fundTotalNavCents += line.endingNavCents;
            snap.fundDeltaNavCents += line.deltaNavCents;
            snap.lines.add(line);
        }

        snap.lines.sort((a, b) -> Integer.compare(a.accountId, b.accountId));
        return snap;
    }

    public NavSnapshot rollForward(NavSnapshot prior, BatchWireFrame frame) {
        if (prior != null && prior.status == Status.OK) {
            for (NavLine line : prior.lines) {
                baselineNavCents.put(line.accountId, line.endingNavCents);
            }
        }
        return compute(frame);
    }

    public boolean violatesMinNav(NavSnapshot snap, long minNavCents) {
        if (snap == null) {
            return true;
        }
        for (NavLine line : snap.lines) {
            if (line.endingNavCents < minNavCents) {
                return true;
            }
        }
        return false;
    }

    public long aggregateFees(NavSnapshot snap) {
        long total = 0;
        if (snap == null) {
            return total;
        }
        for (NavLine line : snap.lines) {
            total += line.feeAccrualCents;
        }
        return total;
    }

    public long aggregateHaircuts(NavSnapshot snap) {
        long total = 0;
        if (snap == null) {
            return total;
        }
        for (NavLine line : snap.lines) {
            total += line.haircutCents;
        }
        return total;
    }

    public NavLine lineForAccount(NavSnapshot snap, int accountId) {
        if (snap == null) {
            return null;
        }
        for (NavLine line : snap.lines) {
            if (line.accountId == accountId) {
                return line;
            }
        }
        return null;
    }

    private long computeFee(long notionalCents) {
        if (notionalCents <= 0 || feeRateBp <= 0) {
            return 0;
        }
        return (notionalCents * feeRateBp) / 10000L;
    }

    private long computeHaircut(long notionalCents, int symbolId) {
        if (notionalCents <= 0 || haircutRateBp <= 0) {
            return 0;
        }
        long symbolAdj = (symbolId % 7L) * 5L;
        long effectiveBp = haircutRateBp + symbolAdj;
        return (notionalCents * effectiveBp) / 10000L;
    }

    public Map<Integer, Long> exportEndingNav(NavSnapshot snap) {
        Map<Integer, Long> out = new HashMap<>();
        if (snap == null) {
            return out;
        }
        for (NavLine line : snap.lines) {
            out.put(line.accountId, line.endingNavCents);
        }
        return out;
    }
}
