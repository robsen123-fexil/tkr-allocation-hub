#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {

enum class ComplianceRuleKind : std::uint8_t {
  kUnknown = 0,
  kConcentrationLimit,
  kRestrictedSymbol,
  kWashSaleWindow,
  kShortLocate,
  kAccountBlock,
  kNotionalCap,
  kSectorLimit,
  kVelocityCheck,
  kGeoRestriction,
};

struct ComplianceRule {
  ComplianceRuleKind kind;
  std::uint32_t rule_id;
  std::uint32_t threshold_value;
  std::uint32_t symbol_id;
  std::uint32_t account_id;
  std::uint32_t sector_id;
  std::string geo_code;
  std::string description;
  bool enabled;
};

struct ComplianceViolation {
  ComplianceRuleKind rule_kind;
  std::uint32_t rule_id;
  std::uint32_t record_id;
  std::uint32_t account_id;
  std::string reason;
};

struct ComplianceRuleEngineConfig {
  std::uint32_t desk_id;
  bool fail_fast;
};

struct ComplianceRuleEngineResult {
  Status status;
  std::uint32_t records_evaluated;
  std::uint32_t rejected_records;
  std::vector<ComplianceViolation> violations;
};

class ComplianceRuleEngine {
 public:
  explicit ComplianceRuleEngine(ComplianceRuleEngineConfig config);

  void AddRule(const ComplianceRule& rule);
  void RemoveRule(std::uint32_t rule_id);
  void ClearRules();

  ComplianceRuleEngineResult Evaluate(const BatchWireFrame& frame);

  Status EvaluateConcentration(const WireBatchRecord& rec,
                               const BatchWireFrame& frame,
                               ComplianceViolation* out) const;
  Status EvaluateRestrictedSymbol(const WireBatchRecord& rec,
                                  ComplianceViolation* out) const;
  Status EvaluateWashSale(const WireBatchRecord& rec,
                          ComplianceViolation* out) const;
  Status EvaluateShortLocate(const WireBatchRecord& rec,
                             ComplianceViolation* out) const;
  Status EvaluateAccountBlock(const WireBatchRecord& rec,
                              ComplianceViolation* out) const;
  Status EvaluateNotionalCap(const WireBatchRecord& rec,
                             ComplianceViolation* out) const;
  Status EvaluateSectorLimit(const WireBatchRecord& rec,
                             const BatchWireFrame& frame,
                             ComplianceViolation* out) const;
  Status EvaluateVelocityCheck(const WireBatchRecord& rec,
                               ComplianceViolation* out) const;
  Status EvaluateGeoRestriction(const WireBatchRecord& rec,
                                ComplianceViolation* out) const;

 private:
  Status ApplyRule(const ComplianceRule& rule, const WireBatchRecord& rec,
                   const BatchWireFrame& frame,
                   ComplianceViolation* out) const;
  void LoadDefaultRules();

  ComplianceRuleEngineConfig config_;
  std::vector<ComplianceRule> rules_;
  std::vector<std::uint32_t> recent_trade_accounts_;
  std::unordered_map<std::uint32_t, std::uint32_t> account_trade_velocity_;
};

}  // namespace desk
}  // namespace tkr
