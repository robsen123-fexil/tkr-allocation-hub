#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

enum class FeeScheduleKind : std::uint8_t {
  kFlatPerTrade = 0,
  kTieredNotional,
  kPerShareMilli,
  kMonthlyMinimum,
  kPerformanceFee,
};

struct FeeTier {
  std::int64_t notional_floor_cents;
  std::int64_t notional_ceiling_cents;
  std::uint32_t rate_bp;
  std::int64_t flat_fee_cents;
};

struct FeeSchedule {
  FeeScheduleKind kind;
  std::uint32_t schedule_id;
  std::uint32_t account_id;
  std::vector<FeeTier> tiers;
  std::int64_t monthly_minimum_cents;
  std::uint32_t per_share_milli_cents;
};

struct AccrualEntry {
  std::uint32_t account_id;
  std::uint32_t schedule_id;
  std::int64_t accrued_cents;
  std::int64_t notional_basis_cents;
  std::uint32_t trade_count;
  std::uint32_t accrual_date_yyyymmdd;
};

struct FeeAccrualEngineConfig {
  std::uint32_t desk_id;
  std::uint32_t accrual_date_yyyymmdd;
};

struct FeeAccrualEngineResult {
  Status status;
  std::vector<AccrualEntry> entries;
  std::int64_t total_accrued_cents;
  std::uint32_t schedules_applied;
};

class FeeAccrualEngine {
 public:
  explicit FeeAccrualEngine(FeeAccrualEngineConfig config);

  void RegisterSchedule(const FeeSchedule& schedule);
  void ClearSchedules();

  FeeAccrualEngineResult AccrueBatch(const BatchWireFrame& frame);
  std::int64_t ComputeTradeFee(const FeeSchedule& schedule,
                               std::uint32_t qty_milli,
                               std::int64_t notional_cents) const;

  const std::vector<AccrualEntry>& Ledger() const { return ledger_; }

 private:
  std::int64_t AccrueFlatPerTrade(const FeeSchedule& schedule) const;
  std::int64_t AccrueTieredNotional(const FeeSchedule& schedule,
                                    std::int64_t notional_cents) const;
  std::int64_t AccruePerShareMilli(const FeeSchedule& schedule,
                                   std::uint32_t qty_milli) const;
  std::int64_t AccrueMonthlyMinimum(const FeeSchedule& schedule,
                                    std::int64_t running_total) const;
  void UpsertLedger(std::uint32_t account_id, std::uint32_t schedule_id,
                    std::int64_t fee_cents, std::int64_t notional_cents);
  void LoadDefaultSchedules();

  FeeAccrualEngineConfig config_;
  std::vector<FeeSchedule> schedules_;
  std::vector<AccrualEntry> ledger_;
};

}  // namespace desk
}  // namespace tkr
