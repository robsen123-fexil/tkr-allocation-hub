#include "tkr/desk/tax_lot_matcher.h"

#include <algorithm>

namespace tkr {
namespace desk {

TaxLotMatcher::TaxLotMatcher(TaxLotMatcherConfig config) : config_(config) {}

void TaxLotMatcher::OpenLot(const TaxLot& lot) { open_lots_.push_back(lot); }

void TaxLotMatcher::ClearLots() { open_lots_.clear(); }

TaxLot* TaxLotMatcher::FindLot(std::uint32_t lot_id) {
  for (TaxLot& lot : open_lots_) {
    if (lot.lot_id == lot_id && !lot.closed) {
      return &lot;
    }
  }
  return nullptr;
}

void TaxLotMatcher::SortLotsByAcquireDate(bool ascending) {
  std::sort(open_lots_.begin(), open_lots_.end(),
            [ascending](const TaxLot& a, const TaxLot& b) {
              if (ascending) {
                return a.acquire_date_yyyymmdd < b.acquire_date_yyyymmdd;
              }
              return a.acquire_date_yyyymmdd > b.acquire_date_yyyymmdd;
            });
}

void TaxLotMatcher::SortLotsByCostBasis(bool highest_first) {
  std::sort(open_lots_.begin(), open_lots_.end(),
            [highest_first](const TaxLot& a, const TaxLot& b) {
              if (highest_first) {
                return a.cost_basis_cents > b.cost_basis_cents;
              }
              return a.cost_basis_cents < b.cost_basis_cents;
            });
}

std::int64_t TaxLotMatcher::ComputeGainLoss(std::int64_t proceeds_cents,
                                            std::int64_t cost_cents) const {
  return proceeds_cents - cost_cents;
}

TaxLotMatcherResult TaxLotMatcher::MatchFifo(
    const TaxLotMatchRequest& request) {
  TaxLotMatcherResult result{};
  result.status = Status::kOk;

  SortLotsByAcquireDate(true);
  std::int64_t remaining = request.sell_qty_milli;

  for (TaxLot& lot : open_lots_) {
    if (lot.closed || lot.account_id != request.account_id ||
        lot.symbol_id != request.symbol_id) {
      continue;
    }

    if (remaining <= 0) {
      break;
    }

    const std::int64_t matched =
        (lot.qty_milli <= remaining) ? lot.qty_milli : remaining;
    const std::int64_t cost_per_unit =
        lot.qty_milli > 0 ? lot.cost_basis_cents / lot.qty_milli : 0;
    const std::int64_t matched_cost = cost_per_unit * matched;

    TaxLotMatchPair pair{};
    pair.lot_id = lot.lot_id;
    pair.matched_qty_milli = matched;
    pair.cost_basis_cents = matched_cost;
    pair.gain_loss_cents = ComputeGainLoss(0, matched_cost);
    result.matches.push_back(pair);

    lot.qty_milli -= matched;
    if (lot.qty_milli == 0) {
      lot.closed = true;
    }

    remaining -= matched;
    result.total_matched_qty += matched;
    result.total_gain_loss_cents += pair.gain_loss_cents;
  }

  result.remaining_qty = remaining;
  return result;
}

TaxLotMatcherResult TaxLotMatcher::MatchLifo(
    const TaxLotMatchRequest& request) {
  SortLotsByAcquireDate(false);
  return MatchFifo(request);
}

TaxLotMatcherResult TaxLotMatcher::MatchHifo(
    const TaxLotMatchRequest& request) {
  SortLotsByCostBasis(true);
  return MatchFifo(request);
}

TaxLotMatcherResult TaxLotMatcher::MatchSpecific(
    const TaxLotMatchRequest& request) {
  TaxLotMatcherResult result{};
  TaxLot* lot = FindLot(request.specific_lot_id);
  if (lot == nullptr) {
    result.status = Status::kBoundsError;
    return result;
  }

  const std::int64_t matched =
      (lot->qty_milli <= request.sell_qty_milli) ? lot->qty_milli
                                                 : request.sell_qty_milli;

  TaxLotMatchPair pair{};
  pair.lot_id = lot->lot_id;
  pair.matched_qty_milli = matched;
  pair.cost_basis_cents =
      (lot->cost_basis_cents * matched) /
      (lot->qty_milli > 0 ? lot->qty_milli : 1);
  pair.gain_loss_cents = ComputeGainLoss(0, pair.cost_basis_cents);
  result.matches.push_back(pair);

  lot->qty_milli -= matched;
  if (lot->qty_milli == 0) {
    lot->closed = true;
  }

  result.total_matched_qty = matched;
  result.total_gain_loss_cents = pair.gain_loss_cents;
  result.remaining_qty = request.sell_qty_milli - matched;
  result.status = Status::kOk;
  return result;
}

TaxLotMatcherResult TaxLotMatcher::MatchAverageCost(
    const TaxLotMatchRequest& request) {
  TaxLotMatcherResult result{};
  result.status = Status::kOk;

  std::int64_t total_qty = 0;
  std::int64_t total_cost = 0;
  for (const TaxLot& lot : open_lots_) {
    if (lot.closed || lot.account_id != request.account_id ||
        lot.symbol_id != request.symbol_id) {
      continue;
    }
    total_qty += lot.qty_milli;
    total_cost += lot.cost_basis_cents;
  }

  if (total_qty == 0) {
    result.status = Status::kBoundsError;
    return result;
  }

  const std::int64_t matched = request.sell_qty_milli;
  const std::int64_t avg_cost = (total_cost * matched) / total_qty;

  TaxLotMatchPair pair{};
  pair.lot_id = 0;
  pair.matched_qty_milli = matched;
  pair.cost_basis_cents = avg_cost;
  pair.gain_loss_cents = ComputeGainLoss(0, avg_cost);
  result.matches.push_back(pair);
  result.total_matched_qty = matched;
  result.total_gain_loss_cents = avg_cost;

  std::int64_t to_reduce = matched;
  for (TaxLot& lot : open_lots_) {
    if (lot.closed || lot.account_id != request.account_id ||
        lot.symbol_id != request.symbol_id) {
      continue;
    }
    const std::int64_t reduce =
        (lot.qty_milli <= to_reduce) ? lot.qty_milli : to_reduce;
    lot.qty_milli -= reduce;
    lot.cost_basis_cents -= (avg_cost * reduce) / matched;
    if (lot.qty_milli == 0) {
      lot.closed = true;
    }
    to_reduce -= reduce;
    if (to_reduce == 0) {
      break;
    }
  }

  return result;
}

TaxLotMatcherResult TaxLotMatcher::Match(const TaxLotMatchRequest& request) {
  switch (request.method) {
    case TaxLotMatchMethod::kLifo:
      return MatchLifo(request);
    case TaxLotMatchMethod::kHifo:
      return MatchHifo(request);
    case TaxLotMatchMethod::kSpecificLot:
      return MatchSpecific(request);
    case TaxLotMatchMethod::kAverageCost:
      return MatchAverageCost(request);
    case TaxLotMatchMethod::kFifo:
    default:
      return MatchFifo(request);
  }
}

TaxLotMatcherResult TaxLotMatcher::MatchBatch(const BatchWireFrame& frame) {
  TaxLotMatcherResult combined{};
  combined.status = Status::kOk;

  for (const WireBatchRecord& rec : frame.records) {
    TaxLotMatchRequest req{};
    req.account_id = rec.account_id;
    req.symbol_id = rec.symbol_id;
    req.sell_qty_milli = static_cast<std::int64_t>(rec.qty_milli);
    req.method = config_.default_method;

    TaxLotMatcherResult partial = Match(req);
    if (partial.status != Status::kOk) {
      combined.status = partial.status;
      return combined;
    }

    for (const TaxLotMatchPair& pair : partial.matches) {
      combined.matches.push_back(pair);
    }
    combined.total_matched_qty += partial.total_matched_qty;
    combined.total_gain_loss_cents += partial.total_gain_loss_cents;
  }

  return combined;
}

}  // namespace desk
}  // namespace tkr
