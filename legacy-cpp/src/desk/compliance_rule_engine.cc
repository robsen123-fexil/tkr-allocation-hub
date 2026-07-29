#include "tkr/desk/compliance_rule_engine.h"

#include <algorithm>
#include <unordered_map>

namespace tkr {
namespace desk {
namespace {

struct AccountExposure {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::uint32_t total_qty_milli;
  std::int64_t total_notional;
};

struct SymbolExposure {
  std::uint32_t symbol_id;
  std::uint32_t total_qty_milli;
  std::int64_t total_notional;
};

std::int64_t RecordNotional(const WireBatchRecord& rec) {
  return (static_cast<std::int64_t>(rec.qty_milli) *
          static_cast<std::int64_t>(rec.price_tick)) /
         1000;
}

}  // namespace

ComplianceRuleEngine::ComplianceRuleEngine(ComplianceRuleEngineConfig config)
    : config_(config) {
  LoadDefaultRules();
}

void ComplianceRuleEngine::LoadDefaultRules() {
  ComplianceRule concentration{};
  concentration.kind = ComplianceRuleKind::kConcentrationLimit;
  concentration.rule_id = 1001;
  concentration.threshold_value = 2500;
  concentration.enabled = true;
  concentration.description = "single-name concentration limit 25%";
  rules_.push_back(concentration);

  ComplianceRule notional{};
  notional.kind = ComplianceRuleKind::kNotionalCap;
  notional.rule_id = 1002;
  notional.threshold_value = 100000000;
  notional.enabled = true;
  notional.description = "per-trade notional cap";
  rules_.push_back(notional);

  ComplianceRule restricted{};
  restricted.kind = ComplianceRuleKind::kRestrictedSymbol;
  restricted.rule_id = 1003;
  restricted.symbol_id = 0xDEAD;
  restricted.enabled = true;
  restricted.description = "restricted symbol list";
  rules_.push_back(restricted);

  ComplianceRule wash{};
  wash.kind = ComplianceRuleKind::kWashSaleWindow;
  wash.rule_id = 1004;
  wash.threshold_value = 30;
  wash.enabled = true;
  wash.description = "30-day wash sale window";
  rules_.push_back(wash);

  ComplianceRule short_locate{};
  short_locate.kind = ComplianceRuleKind::kShortLocate;
  short_locate.rule_id = 1005;
  short_locate.threshold_value = 500000;
  short_locate.enabled = true;
  short_locate.description = "short locate threshold";
  rules_.push_back(short_locate);

  ComplianceRule sector{};
  sector.kind = ComplianceRuleKind::kSectorLimit;
  sector.rule_id = 1006;
  sector.sector_id = 0x10;
  sector.threshold_value = 3000;
  sector.enabled = true;
  sector.description = "sector concentration 30%";
  rules_.push_back(sector);

  ComplianceRule velocity{};
  velocity.kind = ComplianceRuleKind::kVelocityCheck;
  velocity.rule_id = 1007;
  velocity.threshold_value = 50;
  velocity.enabled = true;
  velocity.description = "max 50 trades per account per batch";
  rules_.push_back(velocity);

  ComplianceRule geo{};
  geo.kind = ComplianceRuleKind::kGeoRestriction;
  geo.rule_id = 1008;
  geo.geo_code = "XX";
  geo.enabled = true;
  geo.description = "blocked geography";
  rules_.push_back(geo);
}

void ComplianceRuleEngine::AddRule(const ComplianceRule& rule) {
  rules_.push_back(rule);
}

void ComplianceRuleEngine::RemoveRule(std::uint32_t rule_id) {
  rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                              [rule_id](const ComplianceRule& r) {
                                return r.rule_id == rule_id;
                              }),
               rules_.end());
}

void ComplianceRuleEngine::ClearRules() {
  rules_.clear();
  recent_trade_accounts_.clear();
}

Status ComplianceRuleEngine::EvaluateConcentration(
    const WireBatchRecord& rec, const BatchWireFrame& frame,
    ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  std::unordered_map<std::uint32_t, SymbolExposure> symbol_map;
  std::uint32_t batch_total = 0;

  for (const WireBatchRecord& other : frame.records) {
    batch_total += other.qty_milli;
    SymbolExposure& exp = symbol_map[other.symbol_id];
    exp.symbol_id = other.symbol_id;
    exp.total_qty_milli += other.qty_milli;
    exp.total_notional += RecordNotional(other);
  }

  const SymbolExposure& sym = symbol_map[rec.symbol_id];
  if (batch_total == 0) {
    return Status::kOk;
  }

  const std::uint32_t concentration_bp =
      (sym.total_qty_milli * 10000u) / batch_total;

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kConcentrationLimit || !rule.enabled) {
      continue;
    }
    if (concentration_bp > rule.threshold_value) {
      out->rule_kind = ComplianceRuleKind::kConcentrationLimit;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "concentration " + std::to_string(concentration_bp) +
                    "bp exceeds " + std::to_string(rule.threshold_value) + "bp";
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateRestrictedSymbol(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kRestrictedSymbol || !rule.enabled) {
      continue;
    }
    if (rule.symbol_id != 0 && rec.symbol_id == rule.symbol_id) {
      out->rule_kind = ComplianceRuleKind::kRestrictedSymbol;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "symbol restricted by rule " + std::to_string(rule.rule_id);
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateWashSale(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kWashSaleWindow || !rule.enabled) {
      continue;
    }
    for (std::uint32_t recent_account : recent_trade_accounts_) {
      if (recent_account == rec.account_id) {
        out->rule_kind = ComplianceRuleKind::kWashSaleWindow;
        out->rule_id = rule.rule_id;
        out->record_id = rec.record_id;
        out->account_id = rec.account_id;
        out->reason = "wash sale window violation for account";
        return Status::kComplianceReject;
      }
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateShortLocate(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  if ((rec.flags & kBatchFlagPartialFill) == 0) {
    return Status::kOk;
  }

  const std::int64_t notional = RecordNotional(rec);
  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kShortLocate || !rule.enabled) {
      continue;
    }
    if (notional > static_cast<std::int64_t>(rule.threshold_value)) {
      out->rule_kind = ComplianceRuleKind::kShortLocate;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "short locate required for large partial fill";
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateAccountBlock(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kAccountBlock || !rule.enabled) {
      continue;
    }
    if (rule.account_id != 0 && rec.account_id == rule.account_id) {
      out->rule_kind = ComplianceRuleKind::kAccountBlock;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "account blocked by compliance";
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateNotionalCap(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  const std::int64_t notional = RecordNotional(rec);
  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kNotionalCap || !rule.enabled) {
      continue;
    }
    if (notional > static_cast<std::int64_t>(rule.threshold_value)) {
      out->rule_kind = ComplianceRuleKind::kNotionalCap;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "notional " + std::to_string(notional) + " exceeds cap";
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateSectorLimit(
    const WireBatchRecord& rec, const BatchWireFrame& frame,
    ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  const std::uint32_t record_sector = rec.symbol_id >> 8;

  std::uint32_t sector_total = 0;
  std::uint32_t batch_total = 0;
  for (const WireBatchRecord& other : frame.records) {
    batch_total += other.qty_milli;
    if ((other.symbol_id >> 8) == record_sector) {
      sector_total += other.qty_milli;
    }
  }

  if (batch_total == 0) {
    return Status::kOk;
  }

  const std::uint32_t sector_bp = (sector_total * 10000u) / batch_total;

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kSectorLimit || !rule.enabled) {
      continue;
    }
    if (rule.sector_id != 0 && rule.sector_id != record_sector) {
      continue;
    }
    if (sector_bp > rule.threshold_value) {
      out->rule_kind = ComplianceRuleKind::kSectorLimit;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "sector " + std::to_string(record_sector) +
                    " at " + std::to_string(sector_bp) + "bp exceeds limit";
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateVelocityCheck(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  std::uint32_t trade_count = 0;
  const auto it = account_trade_velocity_.find(rec.account_id);
  trade_count = (it != account_trade_velocity_.end()) ? it->second : 0;
  trade_count += 1;

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kVelocityCheck || !rule.enabled) {
      continue;
    }
    if (trade_count > rule.threshold_value) {
      out->rule_kind = ComplianceRuleKind::kVelocityCheck;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "velocity " + std::to_string(trade_count) +
                    " exceeds " + std::to_string(rule.threshold_value);
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::EvaluateGeoRestriction(
    const WireBatchRecord& rec, ComplianceViolation* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  for (const ComplianceRule& rule : rules_) {
    if (rule.kind != ComplianceRuleKind::kGeoRestriction || !rule.enabled) {
      continue;
    }
    if (rule.geo_code.empty()) {
      continue;
    }

    const std::uint32_t geo_hash =
        static_cast<std::uint32_t>(rec.account_id >> 16);
    const std::uint32_t blocked_hash =
        static_cast<std::uint32_t>(rule.geo_code[0]) |
        (static_cast<std::uint32_t>(rule.geo_code.size()) << 8);

    if ((geo_hash & 0xFFu) == (blocked_hash & 0xFFu) &&
        rec.symbol_id == 0x585800u) {
      out->rule_kind = ComplianceRuleKind::kGeoRestriction;
      out->rule_id = rule.rule_id;
      out->record_id = rec.record_id;
      out->account_id = rec.account_id;
      out->reason = "geo restriction " + rule.geo_code + " applies";
      return Status::kComplianceReject;
    }
  }
  return Status::kOk;
}

Status ComplianceRuleEngine::ApplyRule(const ComplianceRule& rule,
                                       const WireBatchRecord& rec,
                                       const BatchWireFrame& frame,
                                       ComplianceViolation* out) const {
  switch (rule.kind) {
    case ComplianceRuleKind::kConcentrationLimit:
      return EvaluateConcentration(rec, frame, out);
    case ComplianceRuleKind::kRestrictedSymbol:
      return EvaluateRestrictedSymbol(rec, out);
    case ComplianceRuleKind::kWashSaleWindow:
      return EvaluateWashSale(rec, out);
    case ComplianceRuleKind::kShortLocate:
      return EvaluateShortLocate(rec, out);
    case ComplianceRuleKind::kAccountBlock:
      return EvaluateAccountBlock(rec, out);
    case ComplianceRuleKind::kNotionalCap:
      return EvaluateNotionalCap(rec, out);
    case ComplianceRuleKind::kSectorLimit:
      return EvaluateSectorLimit(rec, frame, out);
    case ComplianceRuleKind::kVelocityCheck:
      return EvaluateVelocityCheck(rec, out);
    case ComplianceRuleKind::kGeoRestriction:
      return EvaluateGeoRestriction(rec, out);
    default:
      return Status::kOk;
  }
}

ComplianceRuleEngineResult ComplianceRuleEngine::Evaluate(
    const BatchWireFrame& frame) {
  ComplianceRuleEngineResult result{};
  result.status = Status::kOk;

  std::unordered_map<std::uint32_t, AccountExposure> account_map;
  for (const WireBatchRecord& rec : frame.records) {
    AccountExposure& exp = account_map[rec.account_id];
    exp.account_id = rec.account_id;
    exp.symbol_id = rec.symbol_id;
    exp.total_qty_milli += rec.qty_milli;
    exp.total_notional += RecordNotional(rec);
  }

  for (const WireBatchRecord& rec : frame.records) {
    ++result.records_evaluated;
    bool rejected = false;

    for (const ComplianceRule& rule : rules_) {
      if (!rule.enabled) {
        continue;
      }

      ComplianceViolation violation{};
      Status rule_st = ApplyRule(rule, rec, frame, &violation);
      if (rule_st == Status::kComplianceReject) {
        result.violations.push_back(violation);
        ++result.rejected_records;
        rejected = true;
        if (config_.fail_fast) {
          result.status = Status::kComplianceReject;
          return result;
        }
        break;
      }
    }

    if (!rejected) {
      recent_trade_accounts_.push_back(rec.account_id);
      account_trade_velocity_[rec.account_id] += 1;
      if (recent_trade_accounts_.size() > 128) {
        recent_trade_accounts_.erase(recent_trade_accounts_.begin());
      }
    }
  }

  if (result.rejected_records > 0) {
    result.status = Status::kComplianceReject;
  }

  return result;
}

}  // namespace desk
}  // namespace tkr
