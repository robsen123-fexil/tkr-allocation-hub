#include "tkr/desk/counterparty_limit.h"

#include <algorithm>

namespace tkr {
namespace desk {
namespace {

constexpr std::uint32_t kCounterpartyBase = 50000;

}  // namespace

CounterpartyLimit::CounterpartyLimit(CounterpartyLimitConfig config)
    : config_(config) {
  LoadDefaultLimits();
}

void CounterpartyLimit::LoadDefaultLimits() {
  CounterpartyProfile broker_a{};
  broker_a.counterparty_id = 50001;
  broker_a.name = "BrokerAlpha";
  broker_a.credit_rating_bp = 8500;
  broker_a.approved = true;
  broker_a.parent_id = 0;
  RegisterCounterparty(broker_a);

  CounterpartyProfile broker_b{};
  broker_b.counterparty_id = 50002;
  broker_b.name = "BrokerBeta";
  broker_b.credit_rating_bp = 7200;
  broker_b.approved = true;
  broker_b.parent_id = 0;
  RegisterCounterparty(broker_b);

  ExposureLimit notional{};
  notional.kind = LimitKind::kNotional;
  notional.scope = LimitScope::kPerCounterparty;
  notional.limit_id = 6001;
  notional.counterparty_id = 0;
  notional.max_value = 500000000;
  notional.warning_threshold = 400000000;
  notional.hard_block = true;
  SetLimit(notional);

  ExposureLimit qty{};
  qty.kind = LimitKind::kQuantity;
  qty.scope = LimitScope::kPerSymbol;
  qty.limit_id = 6002;
  qty.counterparty_id = 0;
  qty.max_value = 10000000;
  qty.warning_threshold = 8000000;
  qty.hard_block = true;
  SetLimit(qty);

  ExposureLimit turnover{};
  turnover.kind = LimitKind::kDailyTurnover;
  turnover.scope = LimitScope::kPerCounterparty;
  turnover.limit_id = 6003;
  turnover.counterparty_id = 0;
  turnover.max_value = 1000000000;
  turnover.warning_threshold = 800000000;
  turnover.hard_block = false;
  SetLimit(turnover);
}

void CounterpartyLimit::RegisterCounterparty(const CounterpartyProfile& profile) {
  counterparties_[profile.counterparty_id] = profile;
}

void CounterpartyLimit::SetLimit(const ExposureLimit& limit) {
  for (ExposureLimit& existing : limits_) {
    if (existing.limit_id == limit.limit_id) {
      existing = limit;
      return;
    }
  }
  limits_.push_back(limit);
}

void CounterpartyLimit::RemoveLimit(std::uint32_t limit_id) {
  limits_.erase(std::remove_if(limits_.begin(), limits_.end(),
                               [limit_id](const ExposureLimit& l) {
                                 return l.limit_id == limit_id;
                               }),
                limits_.end());
}

void CounterpartyLimit::ClearLimits() {
  limits_.clear();
  running_exposure_.clear();
}

std::int64_t CounterpartyLimit::ComputeRecordNotional(
    const WireBatchRecord& rec) {
  return (static_cast<std::int64_t>(rec.qty_milli) *
          static_cast<std::int64_t>(rec.price_tick)) /
         1000;
}

std::uint32_t CounterpartyLimit::DeriveCounterpartyId(
    std::uint32_t account_id) {
  return kCounterpartyBase + (account_id % 100);
}

bool CounterpartyLimit::IsCounterpartyApproved(
    std::uint32_t counterparty_id) const {
  const auto it = counterparties_.find(counterparty_id);
  if (it == counterparties_.end()) {
    return true;
  }
  return it->second.approved;
}

std::int64_t CounterpartyLimit::LookupCurrentExposure(
    std::uint32_t counterparty_id, LimitKind kind) const {
  const auto it = running_exposure_.find(counterparty_id);
  if (it == running_exposure_.end()) {
    return 0;
  }
  switch (kind) {
    case LimitKind::kNotional:
      return it->second.notional_cents;
    case LimitKind::kQuantity:
      return it->second.qty_milli;
    case LimitKind::kDailyTurnover:
      return it->second.daily_turnover_cents;
    case LimitKind::kNetExposure:
      return it->second.net_exposure_cents;
    default:
      return 0;
  }
}

ExposureReading CounterpartyLimit::BuildReading(
    const WireBatchRecord& rec) const {
  ExposureReading reading{};
  reading.counterparty_id = DeriveCounterpartyId(rec.account_id);
  reading.account_id = rec.account_id;
  reading.symbol_id = rec.symbol_id;
  reading.notional_cents = ComputeRecordNotional(rec);
  reading.qty_milli = static_cast<std::int64_t>(rec.qty_milli);
  reading.open_order_count = 1;
  reading.daily_turnover_cents = reading.notional_cents;
  reading.net_exposure_cents = reading.notional_cents;
  return reading;
}

ExposureReading CounterpartyLimit::AggregateReadings(
    const std::vector<ExposureReading>& readings,
    std::uint32_t counterparty_id) const {
  ExposureReading agg{};
  agg.counterparty_id = counterparty_id;

  for (const ExposureReading& r : readings) {
    if (r.counterparty_id != counterparty_id) {
      continue;
    }
    agg.notional_cents += r.notional_cents;
    agg.qty_milli += r.qty_milli;
    agg.open_order_count += r.open_order_count;
    agg.daily_turnover_cents += r.daily_turnover_cents;
    agg.net_exposure_cents += r.net_exposure_cents;
  }
  return agg;
}

std::vector<ExposureLimit> CounterpartyLimit::LimitsForCounterparty(
    std::uint32_t counterparty_id) const {
  std::vector<ExposureLimit> result;
  for (const ExposureLimit& limit : limits_) {
    if (limit.counterparty_id == 0 ||
        limit.counterparty_id == counterparty_id) {
      result.push_back(limit);
    }
  }
  return result;
}

Status CounterpartyLimit::CheckLimit(const ExposureLimit& limit,
                                     const ExposureReading& reading,
                                     std::uint32_t record_id,
                                     LimitBreach* out) const {
  if (out == nullptr) {
    return Status::kBoundsError;
  }

  std::int64_t current = 0;
  switch (limit.kind) {
    case LimitKind::kNotional:
      current = reading.notional_cents;
      break;
    case LimitKind::kQuantity:
      current = reading.qty_milli;
      break;
    case LimitKind::kDailyTurnover:
      current = reading.daily_turnover_cents;
      break;
    case LimitKind::kNetExposure:
      current = reading.net_exposure_cents;
      break;
    case LimitKind::kOpenOrders:
      current = reading.open_order_count;
      break;
    default:
      return Status::kOk;
  }

  if (limit.scope == LimitScope::kPerSymbol &&
      limit.symbol_id != 0 && limit.symbol_id != reading.symbol_id) {
    return Status::kOk;
  }

  if (limit.scope == LimitScope::kPerAccount &&
      limit.account_id != 0 && limit.account_id != reading.account_id) {
    return Status::kOk;
  }

  const std::int64_t prior =
      LookupCurrentExposure(reading.counterparty_id, limit.kind);
  current += prior;

  if (current > limit.max_value) {
    out->kind = limit.kind;
    out->limit_id = limit.limit_id;
    out->counterparty_id = reading.counterparty_id;
    out->record_id = record_id;
    out->current_value = current;
    out->limit_value = limit.max_value;
    out->is_warning = false;
    out->reason = "limit breach: current " + std::to_string(current) +
                  " exceeds max " + std::to_string(limit.max_value);
    return limit.hard_block ? Status::kComplianceReject : Status::kOk;
  }

  if (current > limit.warning_threshold) {
    out->kind = limit.kind;
    out->limit_id = limit.limit_id;
    out->counterparty_id = reading.counterparty_id;
    out->record_id = record_id;
    out->current_value = current;
    out->limit_value = limit.warning_threshold;
    out->is_warning = true;
    out->reason = "limit warning: approaching threshold";
  }

  return Status::kOk;
}

CounterpartyLimitResult CounterpartyLimit::EvaluateReading(
    const ExposureReading& reading) {
  CounterpartyLimitResult result{};
  result.status = Status::kOk;

  if (!IsCounterpartyApproved(reading.counterparty_id)) {
    LimitBreach breach{};
    breach.kind = LimitKind::kNotional;
    breach.counterparty_id = reading.counterparty_id;
    breach.is_warning = false;
    breach.reason = "counterparty not approved";
    result.breaches.push_back(breach);
    ++result.breach_count;
    result.status = Status::kComplianceReject;
    return result;
  }

  const std::vector<ExposureLimit> applicable =
      LimitsForCounterparty(reading.counterparty_id);

  for (const ExposureLimit& limit : applicable) {
    LimitBreach breach{};
    Status st = CheckLimit(limit, reading, 0, &breach);
    if (st == Status::kComplianceReject) {
      result.breaches.push_back(breach);
      ++result.breach_count;
      if (config_.enforce_hard_blocks) {
        result.status = Status::kComplianceReject;
      }
    } else if (breach.is_warning) {
      result.breaches.push_back(breach);
      ++result.warning_count;
    }
  }

  ExposureReading& running = running_exposure_[reading.counterparty_id];
  running.counterparty_id = reading.counterparty_id;
  running.notional_cents += reading.notional_cents;
  running.qty_milli += reading.qty_milli;
  running.daily_turnover_cents += reading.daily_turnover_cents;
  running.net_exposure_cents += reading.net_exposure_cents;
  running.open_order_count += reading.open_order_count;

  result.readings.push_back(reading);
  return result;
}

CounterpartyLimitResult CounterpartyLimit::Evaluate(
    const BatchWireFrame& frame) {
  CounterpartyLimitResult result{};
  result.status = Status::kOk;

  std::vector<ExposureReading> batch_readings;
  batch_readings.reserve(frame.records.size());

  for (const WireBatchRecord& rec : frame.records) {
    batch_readings.push_back(BuildReading(rec));
  }

  std::unordered_map<std::uint32_t, ExposureReading> aggregated;
  for (const ExposureReading& r : batch_readings) {
    ExposureReading& agg = aggregated[r.counterparty_id];
    agg.counterparty_id = r.counterparty_id;
    agg.notional_cents += r.notional_cents;
    agg.qty_milli += r.qty_milli;
    agg.daily_turnover_cents += r.daily_turnover_cents;
    agg.net_exposure_cents += r.net_exposure_cents;
    agg.open_order_count += r.open_order_count;
  }

  for (const WireBatchRecord& rec : frame.records) {
    ExposureReading reading = BuildReading(rec);
    CounterpartyLimitResult partial = EvaluateReading(reading);
    for (const LimitBreach& b : partial.breaches) {
      result.breaches.push_back(b);
      if (b.is_warning) {
        ++result.warning_count;
      } else {
        ++result.breach_count;
      }
    }
    if (partial.status == Status::kComplianceReject &&
        config_.enforce_hard_blocks) {
      result.status = Status::kComplianceReject;
    }
    result.readings.push_back(reading);
  }

  if (result.breach_count > 0 && config_.enforce_hard_blocks) {
    result.status = Status::kComplianceReject;
  }

  return result;
}

CounterpartyLimitResult CounterpartyLimit::EvaluateParentAggregate(
    const BatchWireFrame& frame) {
  CounterpartyLimitResult result{};
  result.status = Status::kOk;

  if (!config_.aggregate_parent) {
    return Evaluate(frame);
  }

  std::unordered_map<std::uint32_t, std::vector<ExposureReading>> by_parent;

  for (const WireBatchRecord& rec : frame.records) {
    ExposureReading reading = BuildReading(rec);
    std::uint32_t parent_id = reading.counterparty_id;
    const auto cp_it = counterparties_.find(reading.counterparty_id);
    if (cp_it != counterparties_.end()) {
      parent_id = cp_it->second.parent_id ? cp_it->second.parent_id
                                          : cp_it->second.counterparty_id;
    }
    by_parent[parent_id].push_back(reading);
  }

  for (auto& pair : by_parent) {
    ExposureReading agg = AggregateReadings(pair.second, pair.first);
    CounterpartyLimitResult partial = EvaluateReading(agg);
    for (const LimitBreach& b : partial.breaches) {
      result.breaches.push_back(b);
      if (b.is_warning) {
        ++result.warning_count;
      } else {
        ++result.breach_count;
      }
    }
    result.readings.push_back(agg);
  }

  if (result.breach_count > 0 && config_.enforce_hard_blocks) {
    result.status = Status::kComplianceReject;
  }
  return result;
}

void CounterpartyLimit::ResetRunningExposure() {
  running_exposure_.clear();
}

std::int64_t CounterpartyLimit::TotalDeskExposure(LimitKind kind) const {
  std::int64_t total = 0;
  for (const auto& pair : running_exposure_) {
    switch (kind) {
      case LimitKind::kNotional:
        total += pair.second.notional_cents;
        break;
      case LimitKind::kQuantity:
        total += pair.second.qty_milli;
        break;
      case LimitKind::kDailyTurnover:
        total += pair.second.daily_turnover_cents;
        break;
      default:
        break;
    }
  }
  return total;
}

CounterpartyProfile CounterpartyLimit::LookupProfile(
    std::uint32_t counterparty_id) const {
  const auto it = counterparties_.find(counterparty_id);
  if (it == counterparties_.end()) {
    return CounterpartyProfile{};
  }
  return it->second;
}

}  // namespace desk
}  // namespace tkr
