#include "tkr/desk/lot_splitter.h"

#include <algorithm>

namespace tkr {
namespace desk {

LotSplitter::LotSplitter(LotSplitterConfig config)
    : config_(config), next_fragment_id_(1) {}

std::uint32_t LotSplitter::CountRoundLots(std::uint32_t qty_milli,
                                          std::uint32_t round_lot_milli) {
  if (round_lot_milli == 0) {
    return 0;
  }
  return qty_milli / round_lot_milli;
}

std::uint32_t LotSplitter::OddLotRemainder(std::uint32_t qty_milli,
                                           std::uint32_t round_lot_milli) {
  if (round_lot_milli == 0) {
    return qty_milli;
  }
  return qty_milli % round_lot_milli;
}

void LotSplitter::AssignFragmentIds(std::vector<LotSplitFragment>* fragments) {
  if (fragments == nullptr) {
    return;
  }
  for (LotSplitFragment& frag : *fragments) {
    frag.fragment_id = next_fragment_id_++;
  }
}

LotSplitterResult LotSplitter::SplitProRata(const LotSplitRequest& request) {
  LotSplitterResult result{};
  result.status = Status::kOk;

  if (request.target_account_ids.empty()) {
    result.status = Status::kBoundsError;
    return result;
  }

  std::uint32_t total_weight = 0;
  for (std::uint32_t w : request.target_weights_bp) {
    total_weight += w;
  }
  if (total_weight == 0) {
    total_weight = static_cast<std::uint32_t>(request.target_account_ids.size());
  }

  std::uint32_t allocated = 0;
  for (std::size_t i = 0; i < request.target_account_ids.size(); ++i) {
    const std::uint32_t weight =
        (i < request.target_weights_bp.size()) ? request.target_weights_bp[i] : 1;
    LotSplitFragment frag{};
    frag.parent_lot_id = request.parent_lot_id;
    frag.account_id = request.target_account_ids[i];
    frag.qty_milli =
        (request.total_qty_milli * weight) / total_weight;
    frag.is_odd_lot =
        frag.qty_milli < request.round_lot_milli;
    frag.sequence = static_cast<std::uint32_t>(i);
    allocated += frag.qty_milli;
    result.fragments.push_back(frag);
  }

  std::uint32_t remainder = request.total_qty_milli - allocated;
  result.remainder_milli = remainder;
  if (remainder > 0 && !result.fragments.empty()) {
    result.fragments[0].qty_milli += remainder;
  }

  AssignFragmentIds(&result.fragments);
  for (const LotSplitFragment& frag : result.fragments) {
    if (frag.is_odd_lot) {
      ++result.odd_lot_count;
    } else {
      ++result.round_lot_count;
    }
  }

  return result;
}

LotSplitterResult LotSplitter::SplitSequential(const LotSplitRequest& request) {
  LotSplitterResult result{};
  result.status = Status::kOk;

  std::uint32_t remaining = request.total_qty_milli;
  for (std::size_t i = 0; i < request.target_account_ids.size(); ++i) {
    LotSplitFragment frag{};
    frag.parent_lot_id = request.parent_lot_id;
    frag.account_id = request.target_account_ids[i];
    frag.sequence = static_cast<std::uint32_t>(i);

    const std::uint32_t per_account =
        request.total_qty_milli /
        static_cast<std::uint32_t>(request.target_account_ids.size());
    frag.qty_milli = (remaining >= per_account) ? per_account : remaining;
    remaining -= frag.qty_milli;
    frag.is_odd_lot = frag.qty_milli < request.round_lot_milli;
    result.fragments.push_back(frag);
  }

  if (remaining > 0 && !result.fragments.empty()) {
    result.fragments.back().qty_milli += remaining;
    result.remainder_milli = 0;
  }

  AssignFragmentIds(&result.fragments);
  return result;
}

LotSplitterResult LotSplitter::SplitOddLotFirst(const LotSplitRequest& request) {
  LotSplitterResult result{};
  const std::uint32_t odd = OddLotRemainder(request.total_qty_milli,
                                            request.round_lot_milli);
  const std::uint32_t round_qty = request.total_qty_milli - odd;

  if (odd > 0 && !request.target_account_ids.empty()) {
    LotSplitFragment odd_frag{};
    odd_frag.parent_lot_id = request.parent_lot_id;
    odd_frag.account_id = request.target_account_ids[0];
    odd_frag.qty_milli = odd;
    odd_frag.is_odd_lot = true;
    odd_frag.sequence = 0;
    result.fragments.push_back(odd_frag);
    ++result.odd_lot_count;
  }

  LotSplitRequest round_req = request;
  round_req.total_qty_milli = round_qty;
  LotSplitterResult round_result = SplitProRata(round_req);
  for (LotSplitFragment& frag : round_result.fragments) {
    frag.sequence += 1;
    result.fragments.push_back(frag);
    if (frag.is_odd_lot) {
      ++result.odd_lot_count;
    } else {
      ++result.round_lot_count;
    }
  }

  result.status = round_result.status;
  AssignFragmentIds(&result.fragments);
  return result;
}

LotSplitterResult LotSplitter::SplitRoundLotPreserve(
    const LotSplitRequest& request) {
  LotSplitterResult result{};
  const std::uint32_t round_lot =
      request.round_lot_milli ? request.round_lot_milli
                              : config_.default_round_lot_milli;

  std::uint32_t remaining = request.total_qty_milli;
  for (std::size_t i = 0; i < request.target_account_ids.size() && remaining > 0;
       ++i) {
    LotSplitFragment frag{};
    frag.parent_lot_id = request.parent_lot_id;
    frag.account_id = request.target_account_ids[i];
    frag.sequence = static_cast<std::uint32_t>(i);

    const std::uint32_t full_lots =
        CountRoundLots(remaining, round_lot);
    if (full_lots > 0) {
      frag.qty_milli = round_lot;
      remaining -= round_lot;
    } else {
      frag.qty_milli = remaining;
      remaining = 0;
    }

    frag.is_odd_lot = frag.qty_milli < round_lot;
    if (frag.is_odd_lot) {
      ++result.odd_lot_count;
    } else {
      ++result.round_lot_count;
    }
    result.fragments.push_back(frag);
  }

  result.remainder_milli = remaining;
  result.status = Status::kOk;
  AssignFragmentIds(&result.fragments);
  return result;
}

LotSplitterResult LotSplitter::Split(const LotSplitRequest& request) {
  switch (request.policy) {
    case LotSplitPolicy::kSequentialFill:
      return SplitSequential(request);
    case LotSplitPolicy::kOddLotFirst:
      return SplitOddLotFirst(request);
    case LotSplitPolicy::kRoundLotPreserve:
      return SplitRoundLotPreserve(request);
    case LotSplitPolicy::kProRataByWeight:
    default:
      return SplitProRata(request);
  }
}

LotSplitterResult LotSplitter::SplitBatch(const BatchWireFrame& frame,
                                          LotSplitPolicy policy) {
  LotSplitterResult combined{};
  combined.status = Status::kOk;

  for (const WireBatchRecord& rec : frame.records) {
    LotSplitRequest req{};
    req.parent_lot_id = rec.record_id;
    req.total_qty_milli = rec.qty_milli;
    req.round_lot_milli = config_.default_round_lot_milli;
    req.policy = policy;
    req.target_account_ids.push_back(rec.account_id);
    req.target_weights_bp.push_back(10000);

    LotSplitterResult partial = Split(req);
    if (partial.status != Status::kOk) {
      combined.status = partial.status;
      return combined;
    }

    for (LotSplitFragment& frag : partial.fragments) {
      combined.fragments.push_back(frag);
    }
    combined.odd_lot_count += partial.odd_lot_count;
    combined.round_lot_count += partial.round_lot_count;
  }

  return combined;
}

}  // namespace desk
}  // namespace tkr
