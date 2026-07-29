#include "tkr/desk/restriction_filter.h"

#include <algorithm>

namespace tkr {
namespace desk {

RestrictionFilter::RestrictionFilter(RestrictionFilterConfig config)
    : config_(config) {
  LoadDefaultRestrictions();
}

void RestrictionFilter::LoadDefaultRestrictions() {
  RestrictionRule halt{};
  halt.kind = RestrictionKind::kTradingHalt;
  halt.rule_id = 2001;
  halt.symbol_id = 0xBADC0DE;
  halt.active = true;
  halt.reason = "regulatory trading halt";
  rules_.push_back(halt);
}

void RestrictionFilter::AddRule(const RestrictionRule& rule) {
  rules_.push_back(rule);
}

void RestrictionFilter::RemoveRule(std::uint32_t rule_id) {
  rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                              [rule_id](const RestrictionRule& r) {
                                return r.rule_id == rule_id;
                              }),
               rules_.end());
}

void RestrictionFilter::BlockSymbol(std::uint32_t symbol_id,
                                    const std::string& reason) {
  blocked_symbols_.insert(symbol_id);
  RestrictionRule rule{};
  rule.kind = RestrictionKind::kHardBlock;
  rule.rule_id = 9000 + symbol_id;
  rule.symbol_id = symbol_id;
  rule.reason = reason;
  rule.active = true;
  rules_.push_back(rule);
}

void RestrictionFilter::BlockAccount(std::uint32_t account_id,
                                     const std::string& reason) {
  blocked_accounts_.insert(account_id);
  RestrictionRule rule{};
  rule.kind = RestrictionKind::kHardBlock;
  rule.rule_id = 8000 + account_id;
  rule.account_id = account_id;
  rule.reason = reason;
  rule.active = true;
  rules_.push_back(rule);
}

bool RestrictionFilter::IsSymbolBlocked(std::uint32_t symbol_id) const {
  return blocked_symbols_.count(symbol_id) > 0;
}

bool RestrictionFilter::IsAccountBlocked(std::uint32_t account_id) const {
  return blocked_accounts_.count(account_id) > 0;
}

RestrictionKind RestrictionFilter::ApplyRule(const RestrictionRule& rule,
                                             const WireBatchRecord& rec) const {
  if (!rule.active) {
    return RestrictionKind::kNone;
  }

  switch (rule.kind) {
    case RestrictionKind::kHardBlock:
      if ((rule.symbol_id != 0 && rec.symbol_id == rule.symbol_id) ||
          (rule.account_id != 0 && rec.account_id == rule.account_id)) {
        return RestrictionKind::kHardBlock;
      }
      break;
    case RestrictionKind::kQuantityCap:
      if (rule.symbol_id == rec.symbol_id &&
          rec.qty_milli > rule.max_qty_milli) {
        return RestrictionKind::kQuantityCap;
      }
      break;
    case RestrictionKind::kTradingHalt:
      if (rule.symbol_id == rec.symbol_id) {
        return RestrictionKind::kTradingHalt;
      }
      break;
    case RestrictionKind::kSoftWarning:
      if (rule.account_id == rec.account_id) {
        return RestrictionKind::kSoftWarning;
      }
      break;
    default:
      break;
  }

  return RestrictionKind::kNone;
}

RestrictionKind RestrictionFilter::EvaluateRecord(
    const WireBatchRecord& rec, std::uint32_t* blocking_rule_id) const {
  if (IsSymbolBlocked(rec.symbol_id)) {
    if (blocking_rule_id != nullptr) {
      *blocking_rule_id = 9000 + rec.symbol_id;
    }
    return RestrictionKind::kHardBlock;
  }

  if (IsAccountBlocked(rec.account_id)) {
    if (blocking_rule_id != nullptr) {
      *blocking_rule_id = 8000 + rec.account_id;
    }
    return RestrictionKind::kHardBlock;
  }

  RestrictionKind worst = RestrictionKind::kNone;
  for (const RestrictionRule& rule : rules_) {
    RestrictionKind kind = ApplyRule(rule, rec);
    if (kind == RestrictionKind::kHardBlock ||
        kind == RestrictionKind::kTradingHalt) {
      if (blocking_rule_id != nullptr) {
        *blocking_rule_id = rule.rule_id;
      }
      return kind;
    }
    if (kind == RestrictionKind::kQuantityCap ||
        kind == RestrictionKind::kSoftWarning) {
      worst = kind;
      if (blocking_rule_id != nullptr) {
        *blocking_rule_id = rule.rule_id;
      }
    }
  }

  return worst;
}

RestrictionFilterResult RestrictionFilter::Filter(const BatchWireFrame& frame) {
  RestrictionFilterResult result{};
  result.status = Status::kOk;

  for (const WireBatchRecord& rec : frame.records) {
    FilteredRecord filtered{};
    filtered.record = rec;

    std::uint32_t blocking_rule = 0;
    RestrictionKind kind = EvaluateRecord(rec, &blocking_rule);
    filtered.blocking_kind = kind;
    filtered.blocking_rule_id = blocking_rule;

    switch (kind) {
      case RestrictionKind::kHardBlock:
      case RestrictionKind::kTradingHalt:
        filtered.passed = false;
        ++result.blocked_count;
        if (config_.reject_hard_blocks) {
          result.status = Status::kComplianceReject;
        }
        break;
      case RestrictionKind::kSoftWarning:
        filtered.passed = true;
        ++result.warning_count;
        break;
      case RestrictionKind::kQuantityCap:
        filtered.passed = false;
        ++result.blocked_count;
        break;
      default:
        filtered.passed = true;
        ++result.passed_count;
        break;
    }

    result.filtered.push_back(filtered);
  }

  return result;
}

}  // namespace desk
}  // namespace tkr
