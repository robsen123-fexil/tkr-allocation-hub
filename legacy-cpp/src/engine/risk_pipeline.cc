#include "tkr/engine/risk_pipeline.h"

#include "tkr/desk/benchmark_tracker.h"
#include "tkr/desk/compliance_rule_engine.h"
#include "tkr/desk/counterparty_limit.h"
#include "tkr/desk/haircut_calculator.h"
#include "tkr/desk/margin_aggregator.h"
#include "tkr/desk/restriction_filter.h"
#include "tkr/desk/settlement_calendar.h"

#include <string>

namespace tkr {
namespace engine {

RiskPipeline::RiskPipeline(RiskPipelineConfig config) : config_(config) {}

void RiskPipeline::SetFailFast(bool enabled) { config_.fail_fast = enabled; }

void RiskPipeline::RecordOutcome(RiskPipelineResult* result,
                                 const RiskCheckOutcome& outcome) {
  if (result == nullptr) {
    return;
  }
  result->outcomes.push_back(outcome);
  ++result->checks_run;
  if (outcome.passed) {
    ++result->checks_passed;
  } else {
    ++result->checks_failed;
  }
}

Status RiskPipeline::FinalizeResult(RiskPipelineResult* result) const {
  if (result == nullptr) {
    return Status::kBoundsError;
  }
  if (result->checks_failed > 0) {
    return Status::kComplianceReject;
  }
  return Status::kOk;
}

RiskCheckOutcome RiskPipeline::RunComplianceCheck(
    const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kCompliance;

  ComplianceRuleEngine engine(
      ComplianceRuleEngineConfig{config_.desk_id, config_.fail_fast});
  ComplianceRuleEngineResult res = engine.Evaluate(frame);

  outcome.status = res.status;
  outcome.passed = res.rejected_records == 0;
  outcome.breaches = res.rejected_records;
  outcome.detail = "compliance evaluated " +
                   std::to_string(res.records_evaluated) + " records";
  return outcome;
}

RiskCheckOutcome RiskPipeline::RunMarginCheck(const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kMargin;

  MarginAggregator agg(MarginAggregatorConfig{config_.desk_id, 1000});
  MarginAggregatorResult res = agg.Aggregate(frame);

  outcome.status = res.status;
  outcome.passed = res.breach_count == 0;
  outcome.breaches = res.breach_count;
  outcome.detail = "margin checked " +
                   std::to_string(res.account_checks.size()) + " accounts";
  return outcome;
}

RiskCheckOutcome RiskPipeline::RunHaircutCheck(const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kHaircut;

  HaircutCalculator calc(HaircutCalculatorConfig{config_.desk_id, true});
  std::vector<HaircutInput> inputs;
  inputs.reserve(frame.records.size());

  for (const WireBatchRecord& rec : frame.records) {
    HaircutInput input{};
    input.symbol_id = rec.symbol_id;
    input.asset_class = AssetClass::kEquity;
    input.rating = CreditRating::kA;
    input.market_value_cents =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    inputs.push_back(input);
  }

  HaircutCalculatorSummary summary = calc.Compute(inputs);
  outcome.status = summary.status;
  outcome.passed = summary.status == Status::kOk;
  outcome.detail = "haircut on " + std::to_string(inputs.size()) + " positions";
  return outcome;
}

RiskCheckOutcome RiskPipeline::RunCounterpartyCheck(
    const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kCounterparty;

  CounterpartyLimit limit(CounterpartyLimitConfig{config_.desk_id, true});
  CounterpartyLimitResult res = limit.Evaluate(frame);

  outcome.status = res.status;
  outcome.passed = res.breach_count == 0;
  outcome.breaches = res.breach_count;
  outcome.warnings = res.warning_count;
  outcome.detail = "counterparty limits: " + std::to_string(res.breach_count) +
                   " breaches, " + std::to_string(res.warning_count) +
                   " warnings";
  return outcome;
}

RiskCheckOutcome RiskPipeline::RunSettlementCheck(
    const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kSettlement;

  SettlementCalendar cal(SettlementCalendarConfig{
      MarketRegion::kUsEquity, SettlementCycle::kT2, true});
  SettlementCalendarResult res = cal.ComputeBatch(frame);

  outcome.status = res.status;
  outcome.passed = res.status == Status::kOk;
  outcome.detail = "settlement rolled " +
                   std::to_string(res.instructions.size()) +
                   " instructions T+" +
                   std::to_string(res.business_days_rolled);
  return outcome;
}

RiskCheckOutcome RiskPipeline::RunRestrictionCheck(
    const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kRestriction;

  RestrictionFilter filter(RestrictionFilterConfig{config_.desk_id, true});
  RestrictionFilterResult res = filter.Filter(frame);

  outcome.status = res.status;
  outcome.passed = res.blocked_count == 0;
  outcome.breaches = res.blocked_count;
  outcome.warnings = res.warning_count;
  outcome.detail = "restrictions: " + std::to_string(res.passed_count) +
                   " passed, " + std::to_string(res.blocked_count) + " blocked";
  return outcome;
}

RiskCheckOutcome RiskPipeline::RunBenchmarkCheck(const BatchWireFrame& frame) {
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kBenchmark;

  BenchmarkTracker tracker(BenchmarkTrackerConfig{1, 100});
  BenchmarkTrackerResult res = tracker.TrackBatch(frame);

  outcome.status = res.status;
  outcome.passed = !res.rebalance_needed;
  outcome.detail = "tracking error " + std::to_string(res.tracking_error_bp) +
                   "bp, rebalance=" + (res.rebalance_needed ? "yes" : "no");
  return outcome;
}

RiskPipelineResult RiskPipeline::RunBatchRiskChecks(BatchWireFrame& frame) {
  RiskPipelineResult result{};
  result.status = Status::kOk;

  RiskCheckOutcome compliance = RunComplianceCheck(frame);
  RecordOutcome(&result, compliance);
  result.compliance_cleared = compliance.passed;
  if (!compliance.passed && config_.fail_fast) {
    result.status = compliance.status;
    last_result_ = result;
    return result;
  }

  RiskCheckOutcome restriction = RunRestrictionCheck(frame);
  RecordOutcome(&result, restriction);
  if (!restriction.passed && config_.fail_fast) {
    result.status = restriction.status;
    last_result_ = result;
    return result;
  }

  if (config_.run_counterparty) {
    RiskCheckOutcome counterparty = RunCounterpartyCheck(frame);
    RecordOutcome(&result, counterparty);
    if (!counterparty.passed && config_.fail_fast) {
      result.status = counterparty.status;
      last_result_ = result;
      return result;
    }
  }

  RiskCheckOutcome margin = RunMarginCheck(frame);
  RecordOutcome(&result, margin);
  result.margin_cleared = margin.passed;
  if (!margin.passed &&
      (frame.header.flags & kBatchFlagMarginCheck) != 0 &&
      config_.fail_fast) {
    result.status = Status::kMarginBreach;
    last_result_ = result;
    return result;
  }

  if (config_.run_haircut) {
    RiskCheckOutcome haircut = RunHaircutCheck(frame);
    RecordOutcome(&result, haircut);
  }

  if (config_.run_settlement) {
    RiskCheckOutcome settlement = RunSettlementCheck(frame);
    RecordOutcome(&result, settlement);
  }

  RiskCheckOutcome benchmark = RunBenchmarkCheck(frame);
  RecordOutcome(&result, benchmark);

  result.status = FinalizeResult(&result);
  last_result_ = result;
  return result;
}

RiskPipelineResult RiskPipeline::RunSessionRiskChecks(
    const SessionWireFrame& frame) {
  RiskPipelineResult result{};
  result.status = Status::kOk;

  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kSettlement;
  outcome.passed = frame.header.leg_count <= kMaxSessionLegs;
  outcome.detail = "session " + std::to_string(frame.header.session_id) +
                   " legs=" + std::to_string(frame.header.leg_count);
  RecordOutcome(&result, outcome);

  if (!outcome.passed) {
    result.status = Status::kBoundsError;
  }

  last_result_ = result;
  return result;
}

RiskPipelineResult RiskPipeline::RunPreTradeChecks(BatchWireFrame& frame) {
  RiskPipelineResult result{};
  result.status = Status::kOk;

  RiskCheckOutcome restriction = RunRestrictionCheck(frame);
  RecordOutcome(&result, restriction);
  if (!restriction.passed && config_.fail_fast) {
    result.status = restriction.status;
    return result;
  }

  if (config_.run_counterparty) {
    RiskCheckOutcome counterparty = RunCounterpartyCheck(frame);
    RecordOutcome(&result, counterparty);
    if (!counterparty.passed && config_.fail_fast) {
      result.status = counterparty.status;
      return result;
    }
  }

  RiskCheckOutcome compliance = RunComplianceCheck(frame);
  RecordOutcome(&result, compliance);
  result.compliance_cleared = compliance.passed;

  result.status = FinalizeResult(&result);
  return result;
}

RiskPipelineResult RiskPipeline::RunPostTradeChecks(BatchWireFrame& frame) {
  RiskPipelineResult result{};
  result.status = Status::kOk;

  RiskCheckOutcome margin = RunMarginCheck(frame);
  RecordOutcome(&result, margin);
  result.margin_cleared = margin.passed;

  if (config_.run_haircut) {
    RecordOutcome(&result, RunHaircutCheck(frame));
  }

  if (config_.run_settlement) {
    RecordOutcome(&result, RunSettlementCheck(frame));
  }

  RecordOutcome(&result, RunBenchmarkCheck(frame));

  result.status = FinalizeResult(&result);
  return result;
}

std::uint32_t RiskPipeline::FailedCheckCount() const {
  return last_result_.checks_failed;
}

bool RiskPipeline::AllChecksPassed() const {
  return last_result_.checks_failed == 0;
}

RiskCheckOutcome RiskPipeline::RunFullDeskSweep(BatchWireFrame& frame) {
  RiskPipelineResult pre = RunPreTradeChecks(frame);
  if (pre.status != Status::kOk && config_.fail_fast) {
    RiskCheckOutcome outcome{};
    outcome.kind = RiskCheckKind::kCompliance;
    outcome.status = pre.status;
    outcome.passed = false;
    outcome.detail = "pre-trade sweep failed";
    return outcome;
  }

  RiskPipelineResult post = RunPostTradeChecks(frame);
  RiskCheckOutcome outcome{};
  outcome.kind = RiskCheckKind::kMargin;
  outcome.status = post.status;
  outcome.passed = post.checks_failed == 0;
  outcome.breaches = post.checks_failed;
  outcome.detail = "full desk sweep: " + std::to_string(post.checks_run) +
                   " checks, " + std::to_string(post.checks_passed) + " passed";
  last_result_ = post;
  return outcome;
}

std::string RiskPipeline::BuildSummary() const {
  std::string summary;
  summary.reserve(256);
  summary += "checks_run=" + std::to_string(last_result_.checks_run);
  summary += " passed=" + std::to_string(last_result_.checks_passed);
  summary += " failed=" + std::to_string(last_result_.checks_failed);
  summary += " margin=" + std::string(last_result_.margin_cleared ? "ok" : "fail");
  summary += " compliance=" +
             std::string(last_result_.compliance_cleared ? "ok" : "fail");
  for (const RiskCheckOutcome& o : last_result_.outcomes) {
    summary += " [";
    summary += o.detail;
    summary += "]";
  }
  return summary;
}

}  // namespace engine
}  // namespace tkr
