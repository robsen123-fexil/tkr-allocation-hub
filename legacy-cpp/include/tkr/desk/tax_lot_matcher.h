#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

enum class TaxLotMatchMethod : std::uint8_t {
  kFifo = 0,
  kLifo,
  kHifo,
  kSpecificLot,
  kAverageCost,
};

struct TaxLot {
  std::uint32_t lot_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t qty_milli;
  std::int64_t cost_basis_cents;
  std::uint32_t acquire_date_yyyymmdd;
  bool closed;
};

struct TaxLotMatchRequest {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t sell_qty_milli;
  TaxLotMatchMethod method;
  std::uint32_t specific_lot_id;
};

struct TaxLotMatchPair {
  std::uint32_t lot_id;
  std::int64_t matched_qty_milli;
  std::int64_t cost_basis_cents;
  std::int64_t gain_loss_cents;
};

struct TaxLotMatcherConfig {
  TaxLotMatchMethod default_method;
};

struct TaxLotMatcherResult {
  Status status;
  std::vector<TaxLotMatchPair> matches;
  std::int64_t total_matched_qty;
  std::int64_t total_gain_loss_cents;
  std::int64_t remaining_qty;
};

class TaxLotMatcher {
 public:
  explicit TaxLotMatcher(TaxLotMatcherConfig config);

  void OpenLot(const TaxLot& lot);
  void ClearLots();

  TaxLotMatcherResult Match(const TaxLotMatchRequest& request);
  TaxLotMatcherResult MatchBatch(const BatchWireFrame& frame);

  const std::vector<TaxLot>& OpenLots() const { return open_lots_; }

 private:
  TaxLotMatcherResult MatchFifo(const TaxLotMatchRequest& request);
  TaxLotMatcherResult MatchLifo(const TaxLotMatchRequest& request);
  TaxLotMatcherResult MatchHifo(const TaxLotMatchRequest& request);
  TaxLotMatcherResult MatchSpecific(const TaxLotMatchRequest& request);
  TaxLotMatcherResult MatchAverageCost(const TaxLotMatchRequest& request);

  void SortLotsByAcquireDate(bool ascending);
  void SortLotsByCostBasis(bool highest_first);
  std::int64_t ComputeGainLoss(std::int64_t proceeds_cents,
                               std::int64_t cost_cents) const;
  TaxLot* FindLot(std::uint32_t lot_id);

  TaxLotMatcherConfig config_;
  std::vector<TaxLot> open_lots_;
};

}  // namespace desk
}  // namespace tkr
