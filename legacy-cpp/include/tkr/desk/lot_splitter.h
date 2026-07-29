#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <vector>

namespace tkr {
namespace desk {

enum class LotSplitPolicy : std::uint8_t {
  kProRataByWeight = 0,
  kSequentialFill,
  kOddLotFirst,
  kRoundLotPreserve,
};

struct LotSplitRequest {
  std::uint32_t parent_lot_id;
  std::uint32_t total_qty_milli;
  std::uint32_t round_lot_milli;
  LotSplitPolicy policy;
  std::vector<std::uint32_t> target_account_ids;
  std::vector<std::uint32_t> target_weights_bp;
};

struct LotSplitFragment {
  std::uint32_t fragment_id;
  std::uint32_t parent_lot_id;
  std::uint32_t account_id;
  std::uint32_t qty_milli;
  bool is_odd_lot;
  std::uint32_t sequence;
};

struct LotSplitterConfig {
  std::uint32_t default_round_lot_milli;
  bool preserve_odd_lots;
};

struct LotSplitterResult {
  Status status;
  std::vector<LotSplitFragment> fragments;
  std::uint32_t odd_lot_count;
  std::uint32_t round_lot_count;
  std::uint32_t remainder_milli;
};

class LotSplitter {
 public:
  explicit LotSplitter(LotSplitterConfig config);

  LotSplitterResult Split(const LotSplitRequest& request);
  LotSplitterResult SplitBatch(const BatchWireFrame& frame,
                               LotSplitPolicy policy);

  static std::uint32_t CountRoundLots(std::uint32_t qty_milli,
                                      std::uint32_t round_lot_milli);
  static std::uint32_t OddLotRemainder(std::uint32_t qty_milli,
                                       std::uint32_t round_lot_milli);

 private:
  LotSplitterResult SplitProRata(const LotSplitRequest& request);
  LotSplitterResult SplitSequential(const LotSplitRequest& request);
  LotSplitterResult SplitOddLotFirst(const LotSplitRequest& request);
  LotSplitterResult SplitRoundLotPreserve(const LotSplitRequest& request);
  void AssignFragmentIds(std::vector<LotSplitFragment>* fragments);

  LotSplitterConfig config_;
  std::uint32_t next_fragment_id_;
};

}  // namespace desk
}  // namespace tkr
