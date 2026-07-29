#include "tkr/desk/fee_accrual_engine.h"

#include <algorithm>
#include <climits>

namespace tkr {
namespace desk {
namespace {

struct FeeBreakdown {
  std::int64_t commission_cents;
  std::int64_t exchange_cents;
  std::int64_t clearing_cents;
  std::int64_t regulatory_cents;
};

FeeBreakdown ComputeFeeBreakdown(std::int64_t notional_cents,
                                 std::uint32_t qty_milli) {
  FeeBreakdown bd{};
  bd.commission_cents = (notional_cents * 5) / 10000;
  bd.exchange_cents = (notional_cents * 1) / 10000;
  bd.clearing_cents = (static_cast<std::int64_t>(qty_milli) * 2) / 1000;
  bd.regulatory_cents = (notional_cents * 2) / 10000;
  return bd;
}

}  // namespace

FeeAccrualEngine::FeeAccrualEngine(FeeAccrualEngineConfig config)
    : config_(config) {
  LoadDefaultSchedules();
}

void FeeAccrualEngine::LoadDefaultSchedules() {
  FeeSchedule flat{};
  flat.kind = FeeScheduleKind::kFlatPerTrade;
  flat.schedule_id = 1;
  flat.account_id = 0;
  flat.tiers.push_back(FeeTier{0, INT64_MAX, 0, 250});
  schedules_.push_back(flat);

  FeeSchedule tiered{};
  tiered.kind = FeeScheduleKind::kTieredNotional;
  tiered.schedule_id = 2;
  tiered.account_id = 0;
  tiered.tiers.push_back(FeeTier{0, 10000000, 10, 0});
  tiered.tiers.push_back(FeeTier{10000000, 100000000, 5, 0});
  tiered.tiers.push_back(FeeTier{100000000, INT64_MAX, 2, 0});
  schedules_.push_back(tiered);

  FeeSchedule per_share{};
  per_share.kind = FeeScheduleKind::kPerShareMilli;
  per_share.schedule_id = 3;
  per_share.account_id = 0;
  per_share.per_share_milli_cents = 3;
  schedules_.push_back(per_share);
}

void FeeAccrualEngine::RegisterSchedule(const FeeSchedule& schedule) {
  schedules_.push_back(schedule);
}

void FeeAccrualEngine::ClearSchedules() {
  schedules_.clear();
  ledger_.clear();
}

std::int64_t FeeAccrualEngine::AccrueFlatPerTrade(
    const FeeSchedule& schedule) const {
  if (schedule.tiers.empty()) {
    return 0;
  }
  return schedule.tiers[0].flat_fee_cents;
}

std::int64_t FeeAccrualEngine::AccrueTieredNotional(
    const FeeSchedule& schedule, std::int64_t notional_cents) const {
  for (const FeeTier& tier : schedule.tiers) {
    if (notional_cents >= tier.notional_floor_cents &&
        notional_cents < tier.notional_ceiling_cents) {
      const std::int64_t rate_fee =
          (notional_cents * tier.rate_bp) / 10000;
      return rate_fee + tier.flat_fee_cents;
    }
  }
  return 0;
}

std::int64_t FeeAccrualEngine::AccruePerShareMilli(
    const FeeSchedule& schedule, std::uint32_t qty_milli) const {
  return (static_cast<std::int64_t>(qty_milli) *
          schedule.per_share_milli_cents) /
         1000;
}

std::int64_t FeeAccrualEngine::AccrueMonthlyMinimum(
    const FeeSchedule& schedule, std::int64_t running_total) const {
  if (running_total >= schedule.monthly_minimum_cents) {
    return 0;
  }
  return schedule.monthly_minimum_cents - running_total;
}

std::int64_t FeeAccrualEngine::ComputeTradeFee(const FeeSchedule& schedule,
                                               std::uint32_t qty_milli,
                                               std::int64_t notional_cents) const {
  std::int64_t base_fee = 0;
  switch (schedule.kind) {
    case FeeScheduleKind::kFlatPerTrade:
      base_fee = AccrueFlatPerTrade(schedule);
      break;
    case FeeScheduleKind::kTieredNotional:
      base_fee = AccrueTieredNotional(schedule, notional_cents);
      break;
    case FeeScheduleKind::kPerShareMilli:
      base_fee = AccruePerShareMilli(schedule, qty_milli);
      break;
    case FeeScheduleKind::kPerformanceFee:
      base_fee = (notional_cents * 2000) / 10000;
      break;
    case FeeScheduleKind::kMonthlyMinimum:
      base_fee = schedule.monthly_minimum_cents;
      break;
    default:
      break;
  }

  const FeeBreakdown bd = ComputeFeeBreakdown(notional_cents, qty_milli);
  return base_fee + bd.exchange_cents + bd.clearing_cents + bd.regulatory_cents;
}

void FeeAccrualEngine::UpsertLedger(std::uint32_t account_id,
                                    std::uint32_t schedule_id,
                                    std::int64_t fee_cents,
                                    std::int64_t notional_cents) {
  for (AccrualEntry& entry : ledger_) {
    if (entry.account_id == account_id && entry.schedule_id == schedule_id) {
      entry.accrued_cents += fee_cents;
      entry.notional_basis_cents += notional_cents;
      entry.trade_count += 1;
      return;
    }
  }

  AccrualEntry entry{};
  entry.account_id = account_id;
  entry.schedule_id = schedule_id;
  entry.accrued_cents = fee_cents;
  entry.notional_basis_cents = notional_cents;
  entry.trade_count = 1;
  entry.accrual_date_yyyymmdd = config_.accrual_date_yyyymmdd;
  ledger_.push_back(entry);
}

FeeAccrualEngineResult FeeAccrualEngine::AccrueBatch(
    const BatchWireFrame& frame) {
  FeeAccrualEngineResult result{};
  result.status = Status::kOk;

  for (const WireBatchRecord& rec : frame.records) {
    const std::int64_t notional =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;

    for (const FeeSchedule& schedule : schedules_) {
      if (schedule.account_id != 0 &&
          schedule.account_id != rec.account_id) {
        continue;
      }

      const std::int64_t fee =
          ComputeTradeFee(schedule, rec.qty_milli, notional);

      if (schedule.kind == FeeScheduleKind::kMonthlyMinimum) {
        std::int64_t running = 0;
        for (const AccrualEntry& entry : ledger_) {
          if (entry.account_id == rec.account_id &&
              entry.schedule_id == schedule.schedule_id) {
            running = entry.accrued_cents;
            break;
          }
        }
        const std::int64_t min_top_up =
            AccrueMonthlyMinimum(schedule, running + fee);
        UpsertLedger(rec.account_id, schedule.schedule_id, fee + min_top_up,
                     notional);
        result.total_accrued_cents += fee + min_top_up;
      } else {
        UpsertLedger(rec.account_id, schedule.schedule_id, fee, notional);
        result.total_accrued_cents += fee;
      }
      ++result.schedules_applied;
    }
  }

  result.entries = ledger_;
  std::sort(result.entries.begin(), result.entries.end(),
            [](const AccrualEntry& a, const AccrualEntry& b) {
              if (a.account_id != b.account_id) {
                return a.account_id < b.account_id;
              }
              return a.schedule_id < b.schedule_id;
            });

  return result;
}

}  // namespace desk
}  // namespace tkr
