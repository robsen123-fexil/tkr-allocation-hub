#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {

enum class AssetClass : std::uint8_t {
  kEquity = 0,
  kCorporateBond,
  kGovernmentBond,
  kMunicipalBond,
  kPreferred,
  kConvertible,
};

enum class CreditRating : std::uint8_t {
  kAaa = 0,
  kAa,
  kA,
  kBbb,
  kBb,
  kB,
  kBelowB,
  kNotRated,
};

struct HaircutGridCell {
  AssetClass asset_class;
  CreditRating rating;
  std::uint32_t haircut_bp;
  std::uint32_t concentration_cap_bp;
};

struct HaircutInput {
  std::uint32_t symbol_id;
  AssetClass asset_class;
  CreditRating rating;
  std::int64_t market_value_cents;
  std::uint32_t portfolio_weight_bp;
};

struct HaircutResult {
  std::uint32_t symbol_id;
  std::int64_t gross_value_cents;
  std::int64_t haircut_amount_cents;
  std::int64_t collateral_value_cents;
  std::uint32_t effective_haircut_bp;
};

struct HaircutCalculatorConfig {
  std::uint32_t desk_id;
  bool apply_concentration_penalty;
};

struct HaircutCalculatorSummary {
  Status status;
  std::vector<HaircutResult> results;
  std::int64_t total_gross_cents;
  std::int64_t total_collateral_cents;
  std::int64_t total_haircut_cents;
};

class HaircutCalculator {
 public:
  explicit HaircutCalculator(HaircutCalculatorConfig config);

  HaircutCalculatorSummary Compute(const std::vector<HaircutInput>& inputs);
  HaircutResult ComputeSingle(const HaircutInput& input) const;

  void SetGridCell(const HaircutGridCell& cell);
  void LoadDefaultGrid();

  static std::uint32_t LookupBaseHaircutBp(AssetClass asset_class,
                                           CreditRating rating);
  static std::uint32_t ApplyConcentrationPenalty(std::uint32_t base_haircut_bp,
                                                 std::uint32_t weight_bp);

 private:
  std::uint32_t LookupGridHaircut(AssetClass asset_class,
                                  CreditRating rating) const;
  std::uint64_t GridKey(AssetClass asset_class, CreditRating rating) const;

  HaircutCalculatorConfig config_;
  std::unordered_map<std::uint64_t, HaircutGridCell> grid_;
};

}  // namespace desk
}  // namespace tkr
