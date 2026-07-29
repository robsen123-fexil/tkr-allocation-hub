#include "tkr/desk/haircut_calculator.h"

#include <algorithm>
#include <climits>
#include <unordered_map>

namespace tkr {
namespace desk {
namespace {

struct HaircutTier {
  std::int64_t value_floor_cents;
  std::int64_t value_ceiling_cents;
  std::uint32_t additional_haircut_bp;
};

const HaircutTier kEquityTiers[] = {
    {0, 10000000, 0},
    {10000000, 50000000, 200},
    {50000000, 200000000, 500},
    {200000000, INT64_MAX, 1000},
};

std::uint32_t LookupTierPenalty(std::int64_t market_value_cents) {
  for (const HaircutTier& tier : kEquityTiers) {
    if (market_value_cents >= tier.value_floor_cents &&
        market_value_cents < tier.value_ceiling_cents) {
      return tier.additional_haircut_bp;
    }
  }
  return 0;
}

}  // namespace

HaircutCalculator::HaircutCalculator(HaircutCalculatorConfig config)
    : config_(config) {
  LoadDefaultGrid();
}

std::uint64_t HaircutCalculator::GridKey(AssetClass asset_class,
                                         CreditRating rating) const {
  return (static_cast<std::uint64_t>(asset_class) << 8) |
         static_cast<std::uint64_t>(rating);
}

void HaircutCalculator::SetGridCell(const HaircutGridCell& cell) {
  grid_[GridKey(cell.asset_class, cell.rating)] = cell;
}

std::uint32_t HaircutCalculator::LookupBaseHaircutBp(AssetClass asset_class,
                                                     CreditRating rating) {
  switch (asset_class) {
    case AssetClass::kEquity:
      return 1500;
    case AssetClass::kCorporateBond:
      switch (rating) {
        case CreditRating::kAaa:
          return 200;
        case CreditRating::kAa:
          return 400;
        case CreditRating::kA:
          return 600;
        case CreditRating::kBbb:
          return 1000;
        case CreditRating::kBb:
          return 1500;
        case CreditRating::kB:
          return 2000;
        default:
          return 2500;
      }
    case AssetClass::kGovernmentBond:
      switch (rating) {
        case CreditRating::kAaa:
          return 0;
        case CreditRating::kAa:
          return 50;
        default:
          return 100;
      }
    case AssetClass::kMunicipalBond:
      return 500;
    case AssetClass::kPreferred:
      return 1200;
    case AssetClass::kConvertible:
      return 1800;
    default:
      return 2500;
  }
}

std::uint32_t HaircutCalculator::ApplyConcentrationPenalty(
    std::uint32_t base_haircut_bp, std::uint32_t weight_bp) {
  if (weight_bp <= 1000) {
    return base_haircut_bp;
  }
  const std::uint32_t excess_bp = weight_bp - 1000;
  const std::uint32_t penalty = (excess_bp * 50) / 100;
  return base_haircut_bp + penalty;
}

void HaircutCalculator::LoadDefaultGrid() {
  for (std::uint8_t ac = 0; ac <= static_cast<std::uint8_t>(AssetClass::kConvertible);
       ++ac) {
    for (std::uint8_t rt = 0; rt <= static_cast<std::uint8_t>(CreditRating::kNotRated);
         ++rt) {
      HaircutGridCell cell{};
      cell.asset_class = static_cast<AssetClass>(ac);
      cell.rating = static_cast<CreditRating>(rt);
      cell.haircut_bp = LookupBaseHaircutBp(cell.asset_class, cell.rating);
      cell.concentration_cap_bp = 1000;
      SetGridCell(cell);
    }
  }
}

std::uint32_t HaircutCalculator::LookupGridHaircut(
    AssetClass asset_class, CreditRating rating) const {
  const auto it = grid_.find(GridKey(asset_class, rating));
  if (it == grid_.end()) {
    return LookupBaseHaircutBp(asset_class, rating);
  }
  return it->second.haircut_bp;
}

HaircutResult HaircutCalculator::ComputeSingle(const HaircutInput& input) const {
  HaircutResult result{};
  result.symbol_id = input.symbol_id;
  result.gross_value_cents = input.market_value_cents;

  std::uint32_t haircut_bp = LookupGridHaircut(input.asset_class, input.rating);

  const std::uint32_t tier_penalty =
      LookupTierPenalty(input.market_value_cents);
  haircut_bp += tier_penalty;

  if (config_.apply_concentration_penalty) {
    const auto it = grid_.find(GridKey(input.asset_class, input.rating));
    if (it != grid_.end() &&
        input.portfolio_weight_bp > it->second.concentration_cap_bp) {
      haircut_bp = ApplyConcentrationPenalty(haircut_bp, input.portfolio_weight_bp);
    }
  }

  if (haircut_bp > 10000) {
    haircut_bp = 10000;
  }

  result.effective_haircut_bp = haircut_bp;
  result.haircut_amount_cents =
      (input.market_value_cents * static_cast<std::int64_t>(haircut_bp)) / 10000;
  result.collateral_value_cents =
      input.market_value_cents - result.haircut_amount_cents;

  return result;
}

HaircutCalculatorSummary HaircutCalculator::Compute(
    const std::vector<HaircutInput>& inputs) {
  HaircutCalculatorSummary summary{};
  summary.status = Status::kOk;

  if (inputs.empty()) {
    return summary;
  }

  summary.results.reserve(inputs.size());

  std::int64_t total_gross = 0;
  for (const HaircutInput& input : inputs) {
    total_gross += input.market_value_cents;
  }

  for (const HaircutInput& input : inputs) {
    HaircutInput adjusted = input;
    if (adjusted.portfolio_weight_bp == 0 && total_gross > 0) {
      adjusted.portfolio_weight_bp = static_cast<std::uint32_t>(
          (input.market_value_cents * 10000) / total_gross);
    }

    HaircutResult result = ComputeSingle(adjusted);
    summary.results.push_back(result);
    summary.total_gross_cents += result.gross_value_cents;
    summary.total_haircut_cents += result.haircut_amount_cents;
    summary.total_collateral_cents += result.collateral_value_cents;
  }

  std::sort(summary.results.begin(), summary.results.end(),
            [](const HaircutResult& a, const HaircutResult& b) {
              return a.effective_haircut_bp > b.effective_haircut_bp;
            });

  return summary;
}

}  // namespace desk
}  // namespace tkr
