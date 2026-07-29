#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {

enum class LimitKind : std::uint8_t {
  kNotional = 0,
  kQuantity,
  kOpenOrders,
  kDailyTurnover,
  kNetExposure,
};

enum class LimitScope : std::uint8_t {
  kPerCounterparty = 0,
  kPerAccount,
  kPerSymbol,
  kGlobal,
};

struct CounterpartyProfile {
  std::uint32_t counterparty_id;
  std::string name;
  std::uint32_t credit_rating_bp;
  bool approved;
  std::uint32_t parent_id;
};

struct ExposureLimit {
  LimitKind kind;
  LimitScope scope;
  std::uint32_t limit_id;
  std::uint32_t counterparty_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t max_value;
  std::int64_t warning_threshold;
  bool hard_block;
};

struct ExposureReading {
  std::uint32_t counterparty_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t notional_cents;
  std::int64_t qty_milli;
  std::uint32_t open_order_count;
  std::int64_t daily_turnover_cents;
  std::int64_t net_exposure_cents;
};

struct LimitBreach {
  LimitKind kind;
  std::uint32_t limit_id;
  std::uint32_t counterparty_id;
  std::uint32_t record_id;
  std::int64_t current_value;
  std::int64_t limit_value;
  bool is_warning;
  std::string reason;
};

struct CounterpartyLimitConfig {
  std::uint32_t desk_id;
  bool enforce_hard_blocks;
  bool aggregate_parent;
};

struct CounterpartyLimitResult {
  Status status;
  std::vector<LimitBreach> breaches;
  std::vector<ExposureReading> readings;
  std::uint32_t warning_count;
  std::uint32_t breach_count;
};

class CounterpartyLimit {
 public:
  explicit CounterpartyLimit(CounterpartyLimitConfig config);

  void RegisterCounterparty(const CounterpartyProfile& profile);
  void SetLimit(const ExposureLimit& limit);
  void RemoveLimit(std::uint32_t limit_id);
  void ClearLimits();

  CounterpartyLimitResult Evaluate(const BatchWireFrame& frame);
  CounterpartyLimitResult EvaluateReading(const ExposureReading& reading);
  CounterpartyLimitResult EvaluateParentAggregate(const BatchWireFrame& frame);

  std::int64_t LookupCurrentExposure(std::uint32_t counterparty_id,
                                     LimitKind kind) const;
  bool IsCounterpartyApproved(std::uint32_t counterparty_id) const;
  void ResetRunningExposure();
  std::int64_t TotalDeskExposure(LimitKind kind) const;
  CounterpartyProfile LookupProfile(std::uint32_t counterparty_id) const;

  static std::int64_t ComputeRecordNotional(const WireBatchRecord& rec);
  static std::uint32_t DeriveCounterpartyId(std::uint32_t account_id);

 private:
  Status CheckLimit(const ExposureLimit& limit,
                    const ExposureReading& reading,
                    std::uint32_t record_id,
                    LimitBreach* out) const;
  ExposureReading BuildReading(const WireBatchRecord& rec) const;
  ExposureReading AggregateReadings(
      const std::vector<ExposureReading>& readings,
      std::uint32_t counterparty_id) const;
  void LoadDefaultLimits();
  std::vector<ExposureLimit> LimitsForCounterparty(
      std::uint32_t counterparty_id) const;

  CounterpartyLimitConfig config_;
  std::unordered_map<std::uint32_t, CounterpartyProfile> counterparties_;
  std::vector<ExposureLimit> limits_;
  std::unordered_map<std::uint32_t, ExposureReading> running_exposure_;
};

}  // namespace desk
}  // namespace tkr
