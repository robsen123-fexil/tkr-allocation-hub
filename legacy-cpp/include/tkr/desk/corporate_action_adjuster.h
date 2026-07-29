#pragma once

#include "tkr/desk/position_ledger.h"
#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

enum class CorporateActionKind : std::uint8_t {
  kUnknown = 0,
  kStockSplit,
  kReverseSplit,
  kCashDividend,
  kStockDividend,
  kMerger,
  kSpinOff,
  kRightsIssue,
};

struct CorporateAction {
  CorporateActionKind kind;
  std::uint32_t action_id;
  std::uint32_t symbol_id;
  std::uint32_t effective_date_yyyymmdd;
  std::int64_t ratio_numerator;
  std::int64_t ratio_denominator;
  std::int64_t cash_amount_cents;
  std::uint32_t new_symbol_id;
};

struct AdjustedPosition {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t old_qty_milli;
  std::int64_t new_qty_milli;
  std::int64_t cash_adjustment_cents;
  std::uint32_t action_id;
};

struct CorporateActionAdjusterConfig {
  std::uint32_t desk_id;
};

struct CorporateActionAdjusterResult {
  Status status;
  std::vector<AdjustedPosition> adjustments;
  std::uint32_t actions_applied;
  std::int64_t total_cash_cents;
};

class CorporateActionAdjuster {
 public:
  explicit CorporateActionAdjuster(CorporateActionAdjusterConfig config);

  void RegisterAction(const CorporateAction& action);
  void ClearActions();

  CorporateActionAdjusterResult ApplyActions(
      const std::vector<PositionEntry>& positions);
  CorporateActionAdjusterResult ApplyToBatch(const BatchWireFrame& frame);

  static std::int64_t AdjustQtyForSplit(std::int64_t qty_milli,
                                        std::int64_t numerator,
                                        std::int64_t denominator);
  static std::int64_t ComputeCashDividend(std::int64_t qty_milli,
                                          std::int64_t cash_per_share_cents);

 private:
  AdjustedPosition ApplyStockSplit(const CorporateAction& action,
                                   const PositionEntry& pos) const;
  AdjustedPosition ApplyReverseSplit(const CorporateAction& action,
                                     const PositionEntry& pos) const;
  AdjustedPosition ApplyCashDividend(const CorporateAction& action,
                                     const PositionEntry& pos) const;
  AdjustedPosition ApplyStockDividend(const CorporateAction& action,
                                      const PositionEntry& pos) const;
  AdjustedPosition ApplyMerger(const CorporateAction& action,
                               const PositionEntry& pos) const;
  AdjustedPosition ApplySpinOff(const CorporateAction& action,
                                const PositionEntry& pos) const;

  CorporateActionAdjusterConfig config_;
  std::vector<CorporateAction> pending_actions_;
};

}  // namespace desk
}  // namespace tkr
