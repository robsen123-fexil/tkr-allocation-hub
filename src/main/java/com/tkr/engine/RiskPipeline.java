package com.tkr.engine;

import com.tkr.desk.ComplianceRuleEngine;
import com.tkr.desk.MarginAggregator;
import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.desk.ProRataAllocator;

/** Risk pipeline: compliance then margin checks on batch frames. */
public final class RiskPipeline {

    public static final class RiskPipelineConfig {
        public boolean enforceCompliance = true;
        public boolean enforceMargin = true;
        public boolean haltOnReject = true;
    }

    public static final class RiskPipelineResult {
        public Status status = Status.OK;
        public boolean complianceCleared;
        public boolean marginCleared;
        public AllocationBatchSummary summary = new AllocationBatchSummary();
    }

    private final RiskPipelineConfig config;
    private final ComplianceRuleEngine compliance;
    private final MarginAggregator margin;
    private final ProRataAllocator allocator;

    public RiskPipeline() { this(new RiskPipelineConfig()); }
    public RiskPipeline(RiskPipelineConfig config) {
        this.config = config != null ? config : new RiskPipelineConfig();
        this.compliance = new ComplianceRuleEngine();
        this.margin = new MarginAggregator();
        this.allocator = new ProRataAllocator();
    }

    public RiskPipelineResult evaluateBatch(BatchWireFrame frame) {
        RiskPipelineResult result = new RiskPipelineResult();
        if (frame == null) { result.status = Status.BOUNDS_ERROR; return result; }
        result.summary.batchId = frame.header.deskId;
        result.summary.recordCount = frame.records.size();
        result.summary.flags = frame.header.flags;
        result.summary.deskId = frame.header.deskId;
        long totalQty = 0;
        for (WireBatchRecord rec : frame.records) totalQty += rec.qtyMilli;
        result.summary.totalQtyMilli = (int) totalQty;
        if (config.enforceCompliance || (frame.header.flags & WireTypes.BATCH_FLAG_COMPLIANCE_HOLD) != 0) {
            ComplianceRuleEngine.ComplianceResult cr = compliance.evaluateBatch(frame);
            result.complianceCleared = cr.passed;
            if (!cr.passed && config.haltOnReject) {
                result.status = Status.COMPLIANCE_REJECT;
                return result;
            }
        } else {
            result.complianceCleared = true;
        }
        if (config.enforceMargin || (frame.header.flags & WireTypes.BATCH_FLAG_MARGIN_CHECK) != 0) {
            MarginAggregator.MarginBatchResult mr = margin.aggregateBatch(frame);
            result.marginCleared = mr.allPassed;
            if (!mr.allPassed && config.haltOnReject) {
                result.status = Status.MARGIN_BREACH;
                return result;
            }
        } else {
            result.marginCleared = true;
        }
        result.summary.complianceCleared = result.complianceCleared;
        result.summary.marginCleared = result.marginCleared;
        return result;
    }
}
