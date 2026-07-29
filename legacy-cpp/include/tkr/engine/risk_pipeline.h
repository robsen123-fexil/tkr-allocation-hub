#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace engine {

enum class RiskCheckKind : std::uint8_t {
  kCompliance = 0,
  kMargin,
  kHaircut,
  kCounterparty,
  kSettlement,
  kRestriction,
  kBenchmark,
};

struct RiskCheckOutcome {
  RiskCheckKind kind;
  Status status;
  bool passed;
  std::uint32_t warnings;
  std::uint32_t breaches;
  std::string detail;
};

struct RiskPipelineConfig {
  std::uint32_t desk_id;
  bool fail_fast;
  bool run_haircut;
  bool run_counterparty;
  bool run_settlement;
};

struct RiskPipelineResult {
  Status status;
  std::vector<RiskCheckOutcome> outcomes;
  std::uint32_t checks_run;
  std::uint32_t checks_passed;
  std::uint32_t checks_failed;
  bool margin_cleared;
  bool compliance_cleared;
};

class RiskPipeline {
 public:
  explicit RiskPipeline(RiskPipelineConfig config);

  RiskPipelineResult RunBatchRiskChecks(BatchWireFrame& frame);
  RiskPipelineResult RunSessionRiskChecks(const SessionWireFrame& frame);

  RiskCheckOutcome RunComplianceCheck(const BatchWireFrame& frame);
  RiskCheckOutcome RunMarginCheck(const BatchWireFrame& frame);
  RiskCheckOutcome RunHaircutCheck(const BatchWireFrame& frame);
  RiskCheckOutcome RunCounterpartyCheck(const BatchWireFrame& frame);
  RiskCheckOutcome RunSettlementCheck(const BatchWireFrame& frame);
  RiskCheckOutcome RunRestrictionCheck(const BatchWireFrame& frame);
  RiskCheckOutcome RunBenchmarkCheck(const BatchWireFrame& frame);

  void SetFailFast(bool enabled);
  const RiskPipelineResult& LastResult() const { return last_result_; }

  RiskPipelineResult RunPreTradeChecks(BatchWireFrame& frame);
  RiskPipelineResult RunPostTradeChecks(BatchWireFrame& frame);
  std::uint32_t FailedCheckCount() const;
  bool AllChecksPassed() const;
  RiskCheckOutcome RunFullDeskSweep(BatchWireFrame& frame);
  std::string BuildSummary() const;

 private:
  void RecordOutcome(RiskPipelineResult* result,
                    const RiskCheckOutcome& outcome);
  Status FinalizeResult(RiskPipelineResult* result) const;

  RiskPipelineConfig config_;
  RiskPipelineResult last_result_;
};

}  // namespace engine
}  // namespace tkr
