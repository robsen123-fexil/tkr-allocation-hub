#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace tkr {
namespace desk {

enum class RestrictionKind : std::uint8_t {
  kNone = 0,
  kHardBlock,
  kSoftWarning,
  kQuantityCap,
  kTradingHalt,
  kGeoFence,
};

struct RestrictionRule {
  RestrictionKind kind;
  std::uint32_t rule_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::uint32_t max_qty_milli;
  std::string geo_code;
  std::string reason;
  bool active;
};

struct RestrictionFilterConfig {
  std::uint32_t desk_id;
  bool reject_hard_blocks;
};

struct FilteredRecord {
  WireBatchRecord record;
  bool passed;
  RestrictionKind blocking_kind;
  std::uint32_t blocking_rule_id;
};

struct RestrictionFilterResult {
  Status status;
  std::vector<FilteredRecord> filtered;
  std::uint32_t passed_count;
  std::uint32_t blocked_count;
  std::uint32_t warning_count;
};

class RestrictionFilter {
 public:
  explicit RestrictionFilter(RestrictionFilterConfig config);

  void AddRule(const RestrictionRule& rule);
  void RemoveRule(std::uint32_t rule_id);
  void BlockSymbol(std::uint32_t symbol_id, const std::string& reason);
  void BlockAccount(std::uint32_t account_id, const std::string& reason);

  RestrictionFilterResult Filter(const BatchWireFrame& frame);

  bool IsSymbolBlocked(std::uint32_t symbol_id) const;
  bool IsAccountBlocked(std::uint32_t account_id) const;

 private:
  RestrictionKind EvaluateRecord(const WireBatchRecord& rec,
                                 std::uint32_t* blocking_rule_id) const;
  RestrictionKind ApplyRule(const RestrictionRule& rule,
                            const WireBatchRecord& rec) const;
  void LoadDefaultRestrictions();

  RestrictionFilterConfig config_;
  std::vector<RestrictionRule> rules_;
  std::unordered_set<std::uint32_t> blocked_symbols_;
  std::unordered_set<std::uint32_t> blocked_accounts_;
};

}  // namespace desk
}  // namespace tkr
