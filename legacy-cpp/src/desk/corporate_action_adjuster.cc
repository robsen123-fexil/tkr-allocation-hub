#include "tkr/desk/corporate_action_adjuster.h"

namespace tkr {
namespace desk {

CorporateActionAdjuster::CorporateActionAdjuster(
    CorporateActionAdjusterConfig config)
    : config_(config) {}

void CorporateActionAdjuster::RegisterAction(const CorporateAction& action) {
  pending_actions_.push_back(action);
}

void CorporateActionAdjuster::ClearActions() { pending_actions_.clear(); }

std::int64_t CorporateActionAdjuster::AdjustQtyForSplit(
    std::int64_t qty_milli, std::int64_t numerator,
    std::int64_t denominator) {
  if (denominator == 0) {
    return qty_milli;
  }
  return (qty_milli * numerator) / denominator;
}

std::int64_t CorporateActionAdjuster::ComputeCashDividend(
    std::int64_t qty_milli, std::int64_t cash_per_share_cents) {
  return (qty_milli * cash_per_share_cents) / 1000;
}

AdjustedPosition CorporateActionAdjuster::ApplyStockSplit(
    const CorporateAction& action, const PositionEntry& pos) const {
  AdjustedPosition adj{};
  adj.account_id = pos.account_id;
  adj.symbol_id = pos.symbol_id;
  adj.old_qty_milli = pos.qty_milli;
  adj.new_qty_milli =
      AdjustQtyForSplit(pos.qty_milli, action.ratio_numerator,
                        action.ratio_denominator);
  adj.cash_adjustment_cents = 0;
  adj.action_id = action.action_id;
  return adj;
}

AdjustedPosition CorporateActionAdjuster::ApplyReverseSplit(
    const CorporateAction& action, const PositionEntry& pos) const {
  AdjustedPosition adj = ApplyStockSplit(action, pos);
  adj.action_id = action.action_id;
  return adj;
}

AdjustedPosition CorporateActionAdjuster::ApplyCashDividend(
    const CorporateAction& action, const PositionEntry& pos) const {
  AdjustedPosition adj{};
  adj.account_id = pos.account_id;
  adj.symbol_id = pos.symbol_id;
  adj.old_qty_milli = pos.qty_milli;
  adj.new_qty_milli = pos.qty_milli;
  adj.cash_adjustment_cents =
      ComputeCashDividend(pos.qty_milli, action.cash_amount_cents);
  adj.action_id = action.action_id;
  return adj;
}

AdjustedPosition CorporateActionAdjuster::ApplyStockDividend(
    const CorporateAction& action, const PositionEntry& pos) const {
  AdjustedPosition adj = ApplyStockSplit(action, pos);
  adj.cash_adjustment_cents = 0;
  return adj;
}

AdjustedPosition CorporateActionAdjuster::ApplyMerger(
    const CorporateAction& action, const PositionEntry& pos) const {
  AdjustedPosition adj{};
  adj.account_id = pos.account_id;
  adj.symbol_id = action.new_symbol_id;
  adj.old_qty_milli = pos.qty_milli;
  adj.new_qty_milli =
      AdjustQtyForSplit(pos.qty_milli, action.ratio_numerator,
                        action.ratio_denominator);
  adj.cash_adjustment_cents = action.cash_amount_cents;
  adj.action_id = action.action_id;
  return adj;
}

AdjustedPosition CorporateActionAdjuster::ApplySpinOff(
    const CorporateAction& action, const PositionEntry& pos) const {
  AdjustedPosition adj{};
  adj.account_id = pos.account_id;
  adj.symbol_id = action.new_symbol_id;
  adj.old_qty_milli = pos.qty_milli;
  adj.new_qty_milli =
      AdjustQtyForSplit(pos.qty_milli, action.ratio_numerator,
                        action.ratio_denominator);
  adj.cash_adjustment_cents = 0;
  adj.action_id = action.action_id;
  return adj;
}

CorporateActionAdjusterResult CorporateActionAdjuster::ApplyActions(
    const std::vector<PositionEntry>& positions) {
  CorporateActionAdjusterResult result{};
  result.status = Status::kOk;

  for (const PositionEntry& pos : positions) {
    for (const CorporateAction& action : pending_actions_) {
      if (action.symbol_id != pos.symbol_id) {
        continue;
      }

      AdjustedPosition adj{};
      switch (action.kind) {
        case CorporateActionKind::kStockSplit:
          adj = ApplyStockSplit(action, pos);
          break;
        case CorporateActionKind::kReverseSplit:
          adj = ApplyReverseSplit(action, pos);
          break;
        case CorporateActionKind::kCashDividend:
          adj = ApplyCashDividend(action, pos);
          break;
        case CorporateActionKind::kStockDividend:
          adj = ApplyStockDividend(action, pos);
          break;
        case CorporateActionKind::kMerger:
          adj = ApplyMerger(action, pos);
          break;
        case CorporateActionKind::kSpinOff:
          adj = ApplySpinOff(action, pos);
          break;
        default:
          continue;
      }

      result.adjustments.push_back(adj);
      result.total_cash_cents += adj.cash_adjustment_cents;
      ++result.actions_applied;
    }
  }

  return result;
}

CorporateActionAdjusterResult CorporateActionAdjuster::ApplyToBatch(
    const BatchWireFrame& frame) {
  std::vector<PositionEntry> positions;
  positions.reserve(frame.records.size());

  for (const WireBatchRecord& rec : frame.records) {
    PositionEntry pos{};
    pos.account_id = rec.account_id;
    pos.symbol_id = rec.symbol_id;
    pos.qty_milli = static_cast<std::int64_t>(rec.qty_milli);
    positions.push_back(pos);
  }

  return ApplyActions(positions);
}

}  // namespace desk
}  // namespace tkr
