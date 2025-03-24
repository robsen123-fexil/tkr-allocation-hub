package com.tkr.desk;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;

/**
 * Compliance rule engine: concentration, wash sale, sector, velocity, geo restrictions.
 * Ported from legacy-cpp compliance_rule_engine.cc.
 */
public final class ComplianceRuleEngine {

    public enum ComplianceRuleKind {
        CONCENTRATION_LIMIT,
        RESTRICTED_SYMBOL,
        WASH_SALE_WINDOW,
        SHORT_LOCATE,
        ACCOUNT_BLOCK,
        NOTIONAL_CAP,
        SECTOR_LIMIT,
        VELOCITY_CHECK,
        GEO_RESTRICTION
    }

    public static final class ComplianceRule {
        public ComplianceRuleKind kind;
        public int ruleId;
        public int thresholdValue;
        public int symbolId;
        public int accountId;
        public int sectorId;
        public String geoCode = "";
        public boolean enabled = true;
        public String description = "";
    }

    public static final class ComplianceViolation {
        public ComplianceRuleKind ruleKind;
        public int ruleId;
        public int recordId;
        public int accountId;
        public String reason = "";
    }

    public static final class ComplianceRuleEngineConfig {
        public int deskId;
        public boolean failFast;

        public ComplianceRuleEngineConfig(int deskId, boolean failFast) {
            this.deskId = deskId;
            this.failFast = failFast;
        }
    }

    public static final class ComplianceRuleEngineResult {
        public Status status = Status.OK;
        public int recordsEvaluated;
        public int rejectedRecords;
        public List<ComplianceViolation> violations = new ArrayList<>();
    }

    /** Legacy wrapper result for RiskPipeline / DeskRegistry. */
    public static final class ComplianceResult {
        public boolean passed;
        public List<String> violations = new ArrayList<>();
    }

    private static final class SymbolExposure {
        int symbolId;
        int totalQtyMilli;
        long totalNotional;
    }

    private static final class AccountExposure {
        int accountId;
        int symbolId;
        int totalQtyMilli;
        long totalNotional;
    }

    private final ComplianceRuleEngineConfig config;
    private final List<ComplianceRule> rules = new ArrayList<>();
    private final LinkedList<Integer> recentTradeAccounts = new LinkedList<>();
    private final Map<Integer, Integer> accountTradeVelocity = new HashMap<>();

    public ComplianceRuleEngine() {
        this(new ComplianceRuleEngineConfig(0, true));
    }

    public ComplianceRuleEngine(ComplianceRuleEngineConfig config) {
        this.config = config != null ? config : new ComplianceRuleEngineConfig(0, true);
        loadDefaultRules();
    }

    public void loadDefaultRules() {
        rules.clear();
        addRule(rule(ComplianceRuleKind.CONCENTRATION_LIMIT, 1001, 2500, "single-name concentration limit 25%"));
        addRule(rule(ComplianceRuleKind.NOTIONAL_CAP, 1002, 100_000_000, "per-trade notional cap"));
        ComplianceRule restricted = rule(ComplianceRuleKind.RESTRICTED_SYMBOL, 1003, 0, "restricted symbol list");
        restricted.symbolId = 0xDEAD;
        addRule(restricted);
        addRule(rule(ComplianceRuleKind.WASH_SALE_WINDOW, 1004, 30, "30-day wash sale window"));
        addRule(rule(ComplianceRuleKind.SHORT_LOCATE, 1005, 500_000, "short locate threshold"));
        ComplianceRule sector = rule(ComplianceRuleKind.SECTOR_LIMIT, 1006, 3000, "sector concentration 30%");
        sector.sectorId = 0x10;
        addRule(sector);
        addRule(rule(ComplianceRuleKind.VELOCITY_CHECK, 1007, 50, "max 50 trades per account per batch"));
        ComplianceRule geo = rule(ComplianceRuleKind.GEO_RESTRICTION, 1008, 0, "blocked geography");
        geo.geoCode = "XX";
        addRule(geo);
    }

    private static ComplianceRule rule(ComplianceRuleKind kind, int id, int threshold, String desc) {
        ComplianceRule r = new ComplianceRule();
        r.kind = kind;
        r.ruleId = id;
        r.thresholdValue = threshold;
        r.description = desc;
        r.enabled = true;
        return r;
    }

    public void addRule(ComplianceRule rule) {
        if (rule != null) rules.add(rule);
    }

    public void removeRule(int ruleId) {
        rules.removeIf(r -> r.ruleId == ruleId);
    }

    public void clearRules() {
        rules.clear();
        recentTradeAccounts.clear();
        accountTradeVelocity.clear();
    }

    public List<ComplianceRule> getRules() {
        return new ArrayList<>(rules);
    }

    private static long recordNotional(WireBatchRecord rec) {
        return (long) rec.qtyMilli * (long) rec.priceTick / 1000L;
    }

    public Status evaluateConcentration(WireBatchRecord rec, BatchWireFrame frame, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        Map<Integer, SymbolExposure> symbolMap = new HashMap<>();
        int batchTotal = 0;
        for (WireBatchRecord other : frame.records) {
            batchTotal += other.qtyMilli;
            SymbolExposure exp = symbolMap.computeIfAbsent(other.symbolId, id -> {
                SymbolExposure s = new SymbolExposure();
                s.symbolId = id;
                return s;
            });
            exp.totalQtyMilli += other.qtyMilli;
            exp.totalNotional += recordNotional(other);
        }
        SymbolExposure sym = symbolMap.get(rec.symbolId);
        if (batchTotal == 0 || sym == null) return Status.OK;
        int concentrationBp = sym.totalQtyMilli * 10000 / batchTotal;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.CONCENTRATION_LIMIT || !rule.enabled) continue;
            if (concentrationBp > rule.thresholdValue) {
                out.ruleKind = ComplianceRuleKind.CONCENTRATION_LIMIT;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "concentration " + concentrationBp + "bp exceeds " + rule.thresholdValue + "bp";
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateRestrictedSymbol(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.RESTRICTED_SYMBOL || !rule.enabled) continue;
            if (rule.symbolId != 0 && rec.symbolId == rule.symbolId) {
                out.ruleKind = ComplianceRuleKind.RESTRICTED_SYMBOL;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "symbol restricted by rule " + rule.ruleId;
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateWashSale(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.WASH_SALE_WINDOW || !rule.enabled) continue;
            for (Integer recentAccount : recentTradeAccounts) {
                if (recentAccount == rec.accountId) {
                    out.ruleKind = ComplianceRuleKind.WASH_SALE_WINDOW;
                    out.ruleId = rule.ruleId;
                    out.recordId = rec.recordId;
                    out.accountId = rec.accountId;
                    out.reason = "wash sale window violation for account";
                    return Status.COMPLIANCE_REJECT;
                }
            }
        }
        return Status.OK;
    }

    public Status evaluateShortLocate(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        if ((rec.flags & WireTypes.BATCH_FLAG_PARTIAL_FILL) == 0) return Status.OK;
        long notional = recordNotional(rec);
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.SHORT_LOCATE || !rule.enabled) continue;
            if (notional > rule.thresholdValue) {
                out.ruleKind = ComplianceRuleKind.SHORT_LOCATE;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "short locate required for large partial fill";
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateAccountBlock(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.ACCOUNT_BLOCK || !rule.enabled) continue;
            if (rule.accountId != 0 && rec.accountId == rule.accountId) {
                out.ruleKind = ComplianceRuleKind.ACCOUNT_BLOCK;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "account blocked by compliance";
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateNotionalCap(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        long notional = recordNotional(rec);
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.NOTIONAL_CAP || !rule.enabled) continue;
            if (notional > rule.thresholdValue) {
                out.ruleKind = ComplianceRuleKind.NOTIONAL_CAP;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "notional " + notional + " exceeds cap";
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateSectorLimit(WireBatchRecord rec, BatchWireFrame frame, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        int recordSector = rec.symbolId >> 8;
        int sectorTotal = 0;
        int batchTotal = 0;
        for (WireBatchRecord other : frame.records) {
            batchTotal += other.qtyMilli;
            if ((other.symbolId >> 8) == recordSector) sectorTotal += other.qtyMilli;
        }
        if (batchTotal == 0) return Status.OK;
        int sectorBp = sectorTotal * 10000 / batchTotal;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.SECTOR_LIMIT || !rule.enabled) continue;
            if (rule.sectorId != 0 && rule.sectorId != recordSector) continue;
            if (sectorBp > rule.thresholdValue) {
                out.ruleKind = ComplianceRuleKind.SECTOR_LIMIT;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "sector " + recordSector + " at " + sectorBp + "bp exceeds limit";
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateVelocityCheck(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        int tradeCount = accountTradeVelocity.getOrDefault(rec.accountId, 0) + 1;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.VELOCITY_CHECK || !rule.enabled) continue;
            if (tradeCount > rule.thresholdValue) {
                out.ruleKind = ComplianceRuleKind.VELOCITY_CHECK;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "velocity " + tradeCount + " exceeds " + rule.thresholdValue;
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status evaluateGeoRestriction(WireBatchRecord rec, ComplianceViolation out) {
        if (out == null) return Status.BOUNDS_ERROR;
        for (ComplianceRule rule : rules) {
            if (rule.kind != ComplianceRuleKind.GEO_RESTRICTION || !rule.enabled) continue;
            if (rule.geoCode == null || rule.geoCode.isEmpty()) continue;
            int geoHash = rec.accountId >> 16;
            int blockedHash = rule.geoCode.charAt(0) | (rule.geoCode.length() << 8);
            if ((geoHash & 0xFF) == (blockedHash & 0xFF) && rec.symbolId == 0x585800) {
                out.ruleKind = ComplianceRuleKind.GEO_RESTRICTION;
                out.ruleId = rule.ruleId;
                out.recordId = rec.recordId;
                out.accountId = rec.accountId;
                out.reason = "geo restriction " + rule.geoCode + " applies";
                return Status.COMPLIANCE_REJECT;
            }
        }
        return Status.OK;
    }

    public Status applyRule(ComplianceRule rule, WireBatchRecord rec, BatchWireFrame frame, ComplianceViolation out) {
        return switch (rule.kind) {
            case CONCENTRATION_LIMIT -> evaluateConcentration(rec, frame, out);
            case RESTRICTED_SYMBOL -> evaluateRestrictedSymbol(rec, out);
            case WASH_SALE_WINDOW -> evaluateWashSale(rec, out);
            case SHORT_LOCATE -> evaluateShortLocate(rec, out);
            case ACCOUNT_BLOCK -> evaluateAccountBlock(rec, out);
            case NOTIONAL_CAP -> evaluateNotionalCap(rec, out);
            case SECTOR_LIMIT -> evaluateSectorLimit(rec, frame, out);
            case VELOCITY_CHECK -> evaluateVelocityCheck(rec, out);
            case GEO_RESTRICTION -> evaluateGeoRestriction(rec, out);
        };
    }

    public ComplianceRuleEngineResult evaluate(BatchWireFrame frame) {
        ComplianceRuleEngineResult result = new ComplianceRuleEngineResult();
        result.status = Status.OK;
        if (frame == null) {
            result.status = Status.BOUNDS_ERROR;
            return result;
        }
        Map<Integer, AccountExposure> accountMap = new HashMap<>();
        for (WireBatchRecord rec : frame.records) {
            AccountExposure exp = accountMap.computeIfAbsent(rec.accountId, id -> {
                AccountExposure a = new AccountExposure();
                a.accountId = id;
                return a;
            });
            exp.symbolId = rec.symbolId;
            exp.totalQtyMilli += rec.qtyMilli;
            exp.totalNotional += recordNotional(rec);
        }
        for (WireBatchRecord rec : frame.records) {
            result.recordsEvaluated++;
            boolean rejected = false;
            for (ComplianceRule rule : rules) {
                if (!rule.enabled) continue;
                ComplianceViolation violation = new ComplianceViolation();
                Status ruleSt = applyRule(rule, rec, frame, violation);
                if (ruleSt == Status.COMPLIANCE_REJECT) {
                    result.violations.add(violation);
                    result.rejectedRecords++;
                    rejected = true;
                    if (config.failFast) {
                        result.status = Status.COMPLIANCE_REJECT;
                        return result;
                    }
                    break;
                }
            }
            if (!rejected) {
                recentTradeAccounts.addLast(rec.accountId);
                accountTradeVelocity.merge(rec.accountId, 1, Integer::sum);
                while (recentTradeAccounts.size() > 128) recentTradeAccounts.removeFirst();
            }
        }
        if (result.rejectedRecords > 0) result.status = Status.COMPLIANCE_REJECT;
        return result;
    }

    /** Legacy API used by DeskRegistry and RiskPipeline. */
    public ComplianceResult evaluateBatch(BatchWireFrame frame) {
        ComplianceRuleEngineResult engineResult = evaluate(frame);
        ComplianceResult legacy = new ComplianceResult();
        legacy.passed = engineResult.status == Status.OK;
        for (ComplianceViolation v : engineResult.violations) {
            legacy.violations.add(v.ruleKind.name().toLowerCase() + ":" + v.recordId + ":" + v.reason);
        }
        return legacy;
    }

    public ComplianceResult evaluateRecord(WireBatchRecord rec, BatchWireFrame frame) {
        BatchWireFrame single = new BatchWireFrame();
        single.header = frame.header;
        single.records = List.of(rec);
        return evaluateBatch(single);
    }

    public int countViolationsByPrefix(ComplianceResult result, String prefix) {
        int count = 0;
        for (String v : result.violations) if (v.startsWith(prefix)) count++;
        return count;
    }

    public void addRestrictedSymbol(int symbolId) {
        ComplianceRule r = rule(ComplianceRuleKind.RESTRICTED_SYMBOL, 9000 + symbolId, 0, "dynamic block");
        r.symbolId = symbolId;
        addRule(r);
    }

    public void addRestrictedAccount(int accountId) {
        ComplianceRule r = rule(ComplianceRuleKind.ACCOUNT_BLOCK, 8000 + accountId, 0, "dynamic block");
        r.accountId = accountId;
        addRule(r);
    }

    public void setConcentrationLimitBp(int symbolId, int limitBp) {
        ComplianceRule r = rule(ComplianceRuleKind.CONCENTRATION_LIMIT, 7000 + symbolId, limitBp, "per-symbol concentration");
        r.symbolId = symbolId;
        addRule(r);
    }

    public boolean isSymbolRestricted(int symbolId) {
        for (ComplianceRule rule : rules) {
            if (rule.kind == ComplianceRuleKind.RESTRICTED_SYMBOL && rule.enabled && rule.symbolId == symbolId) return true;
        }
        return false;
    }

    public boolean isAccountRestricted(int accountId) {
        for (ComplianceRule rule : rules) {
            if (rule.kind == ComplianceRuleKind.ACCOUNT_BLOCK && rule.enabled && rule.accountId == accountId) return true;
        }
        return false;
    }

    public Map<Integer, Integer> snapshotVelocity() {
        return new HashMap<>(accountTradeVelocity);
    }

    public void resetVelocityTracking() {
        recentTradeAccounts.clear();
        accountTradeVelocity.clear();
    }

    public int violationCountForRule(int ruleId) {
        int n = 0;
        for (ComplianceViolation v : evaluate(new BatchWireFrame()).violations) {
            if (v.ruleId == ruleId) n++;
        }
        return n;
    }

    public List<ComplianceViolation> filterViolationsByAccount(int accountId, ComplianceRuleEngineResult result) {
        List<ComplianceViolation> filtered = new ArrayList<>();
        for (ComplianceViolation v : result.violations) {
            if (v.accountId == accountId) filtered.add(v);
        }
        return filtered;
    }

    public String summarizeResult(ComplianceRuleEngineResult result) {
        StringBuilder sb = new StringBuilder();
        sb.append("evaluated=").append(result.recordsEvaluated);
        sb.append(" rejected=").append(result.rejectedRecords);
        sb.append(" status=").append(result.status);
        return sb.toString();
    }

    public ComplianceRuleEngineResult evaluateWithRuleFilter(BatchWireFrame frame, ComplianceRuleKind kind) {
        List<ComplianceRule> saved = new ArrayList<>(rules);
        rules.removeIf(r -> r.kind != kind);
        ComplianceRuleEngineResult result = evaluate(frame);
        rules.clear();
        rules.addAll(saved);
        return result;
    }

    public Map<ComplianceRuleKind, Integer> violationCountsByKind(ComplianceRuleEngineResult result) {
        Map<ComplianceRuleKind, Integer> counts = new HashMap<>();
        for (ComplianceViolation v : result.violations) {
            counts.merge(v.ruleKind, 1, Integer::sum);
        }
        return counts;
    }

    public boolean wouldPassExcept(BatchWireFrame frame, ComplianceRuleKind exceptKind) {
        for (WireBatchRecord rec : frame.records) {
            for (ComplianceRule rule : rules) {
                if (!rule.enabled || rule.kind == exceptKind) continue;
                ComplianceViolation violation = new ComplianceViolation();
                if (applyRule(rule, rec, frame, violation) == Status.COMPLIANCE_REJECT) return false;
            }
        }
        return true;
    }
}
