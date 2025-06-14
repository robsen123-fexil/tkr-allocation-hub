package com.tkr.desk;

import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Restriction filter for trading halts, hard blocks, quantity caps.
 * Ported from legacy-cpp restriction_filter.cc.
 */
public final class RestrictionFilter {

    public enum RestrictionKind { NONE, HARD_BLOCK, QUANTITY_CAP, TRADING_HALT, SOFT_WARNING }

    public static final class RestrictionRule {
        public RestrictionKind kind;
        public int ruleId;
        public int symbolId;
        public int accountId;
        public int maxQtyMilli;
        public boolean active = true;
        public String reason = "";
    }

    public static final class FilteredRecord {
        public WireBatchRecord record;
        public RestrictionKind blockingKind = RestrictionKind.NONE;
        public int blockingRuleId;
        public boolean passed = true;
    }

    public static final class RestrictionFilterConfig {
        public int deskId;
        public boolean rejectHardBlocks;

        public RestrictionFilterConfig(int deskId, boolean rejectHardBlocks) {
            this.deskId = deskId;
            this.rejectHardBlocks = rejectHardBlocks;
        }
    }

    public static final class RestrictionFilterResult {
        public Status status = Status.OK;
        public List<FilteredRecord> filtered = new ArrayList<>();
        public int passedCount;
        public int blockedCount;
        public int warningCount;
    }

    private final RestrictionFilterConfig config;
    private final List<RestrictionRule> rules = new ArrayList<>();
    private final Set<Integer> blockedSymbols = new HashSet<>();
    private final Set<Integer> blockedAccounts = new HashSet<>();

    public RestrictionFilter() {
        this(new RestrictionFilterConfig(0, true));
    }

    public RestrictionFilter(RestrictionFilterConfig config) {
        this.config = config != null ? config : new RestrictionFilterConfig(0, true);
        loadDefaultRestrictions();
    }

    public void loadDefaultRestrictions() {
        RestrictionRule halt = new RestrictionRule();
        halt.kind = RestrictionKind.TRADING_HALT;
        halt.ruleId = 2001;
        halt.symbolId = 0xBADC0DE;
        halt.active = true;
        halt.reason = "regulatory trading halt";
        rules.add(halt);
    }

    public void addRule(RestrictionRule rule) {
        if (rule != null) rules.add(rule);
    }

    public void removeRule(int ruleId) {
        rules.removeIf(r -> r.ruleId == ruleId);
    }

    public void blockSymbol(int symbolId, String reason) {
        blockedSymbols.add(symbolId);
        RestrictionRule rule = new RestrictionRule();
        rule.kind = RestrictionKind.HARD_BLOCK;
        rule.ruleId = 9000 + symbolId;
        rule.symbolId = symbolId;
        rule.reason = reason;
        rule.active = true;
        rules.add(rule);
    }

    public void blockAccount(int accountId, String reason) {
        blockedAccounts.add(accountId);
        RestrictionRule rule = new RestrictionRule();
        rule.kind = RestrictionKind.HARD_BLOCK;
        rule.ruleId = 8000 + accountId;
        rule.accountId = accountId;
        rule.reason = reason;
        rule.active = true;
        rules.add(rule);
    }

    public boolean isSymbolBlocked(int symbolId) {
        return blockedSymbols.contains(symbolId);
    }

    public boolean isAccountBlocked(int accountId) {
        return blockedAccounts.contains(accountId);
    }

    public RestrictionKind applyRule(RestrictionRule rule, WireBatchRecord rec) {
        if (!rule.active) return RestrictionKind.NONE;
        return switch (rule.kind) {
            case HARD_BLOCK -> {
                if ((rule.symbolId != 0 && rec.symbolId == rule.symbolId)
                        || (rule.accountId != 0 && rec.accountId == rule.accountId)) {
                    yield RestrictionKind.HARD_BLOCK;
                }
                yield RestrictionKind.NONE;
            }
            case QUANTITY_CAP -> {
                if (rule.symbolId == rec.symbolId && rec.qtyMilli > rule.maxQtyMilli) {
                    yield RestrictionKind.QUANTITY_CAP;
                }
                yield RestrictionKind.NONE;
            }
            case TRADING_HALT -> rule.symbolId == rec.symbolId ? RestrictionKind.TRADING_HALT : RestrictionKind.NONE;
            case SOFT_WARNING -> rule.accountId == rec.accountId ? RestrictionKind.SOFT_WARNING : RestrictionKind.NONE;
            default -> RestrictionKind.NONE;
        };
    }

    public RestrictionKind evaluateRecord(WireBatchRecord rec, int[] blockingRuleId) {
        if (isSymbolBlocked(rec.symbolId)) {
            if (blockingRuleId != null && blockingRuleId.length > 0) blockingRuleId[0] = 9000 + rec.symbolId;
            return RestrictionKind.HARD_BLOCK;
        }
        if (isAccountBlocked(rec.accountId)) {
            if (blockingRuleId != null && blockingRuleId.length > 0) blockingRuleId[0] = 8000 + rec.accountId;
            return RestrictionKind.HARD_BLOCK;
        }
        RestrictionKind worst = RestrictionKind.NONE;
        for (RestrictionRule rule : rules) {
            RestrictionKind kind = applyRule(rule, rec);
            if (kind == RestrictionKind.HARD_BLOCK || kind == RestrictionKind.TRADING_HALT) {
                if (blockingRuleId != null && blockingRuleId.length > 0) blockingRuleId[0] = rule.ruleId;
                return kind;
            }
            if (kind == RestrictionKind.QUANTITY_CAP || kind == RestrictionKind.SOFT_WARNING) {
                worst = kind;
                if (blockingRuleId != null && blockingRuleId.length > 0) blockingRuleId[0] = rule.ruleId;
            }
        }
        return worst;
    }

    public RestrictionFilterResult filter(BatchWireFrame frame) {
        RestrictionFilterResult result = new RestrictionFilterResult();
        result.status = Status.OK;
        if (frame == null) return result;
        for (WireBatchRecord rec : frame.records) {
            FilteredRecord filtered = new FilteredRecord();
            filtered.record = rec;
            int[] blockingRule = new int[1];
            RestrictionKind kind = evaluateRecord(rec, blockingRule);
            filtered.blockingKind = kind;
            filtered.blockingRuleId = blockingRule[0];
            switch (kind) {
                case HARD_BLOCK, TRADING_HALT -> {
                    filtered.passed = false;
                    result.blockedCount++;
                    if (config.rejectHardBlocks) result.status = Status.COMPLIANCE_REJECT;
                }
                case SOFT_WARNING -> {
                    filtered.passed = true;
                    result.warningCount++;
                }
                case QUANTITY_CAP -> {
                    filtered.passed = false;
                    result.blockedCount++;
                }
                default -> {
                    filtered.passed = true;
                    result.passedCount++;
                }
            }
            result.filtered.add(filtered);
        }
        return result;
    }

    public List<RestrictionRule> getRules() {
        return new ArrayList<>(rules);
    }

    public int blockedSymbolCount() {
        return blockedSymbols.size();
    }
}
