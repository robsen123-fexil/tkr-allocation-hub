#pragma once

#include "tkr/desk/pro_rata_allocator.h"
#include "tkr/types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace desk {

struct PositionEntry {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t qty_milli;
  std::int64_t cost_basis_cents;
  std::uint32_t last_trade_date;
  std::uint32_t version;
};

struct PositionLedgerConfig {
  std::uint32_t desk_id;
};

struct PositionLedgerSnapshot {
  std::uint32_t entry_count;
  std::int64_t total_qty_milli;
  std::int64_t total_cost_cents;
};

class PositionLedger {
 public:
  explicit PositionLedger(PositionLedgerConfig config);

  Status ApplyBatch(const BatchWireFrame& frame,
                    const std::vector<ProRataSlice>& slices);
  Status ApplySlice(const ProRataSlice& slice, std::uint32_t symbol_id,
                    std::uint32_t price_tick);

  PositionEntry Lookup(std::uint32_t account_id,
                       std::uint32_t symbol_id) const;
  PositionLedgerSnapshot Snapshot() const;
  const std::vector<PositionEntry>& Entries() const { return entries_; }

  std::vector<PositionEntry> EntriesByAccount(std::uint32_t account_id) const;
  std::int64_t TotalQtyForSymbol(std::uint32_t symbol_id) const;
  Status ReconcileEntry(std::uint32_t account_id, std::uint32_t symbol_id,
                        std::int64_t expected_qty);
  Status MergeEntries(const PositionEntry& other);
  bool HasOpenPosition(std::uint32_t account_id,
                       std::uint32_t symbol_id) const;
  std::int64_t AverageCost(std::uint32_t account_id,
                           std::uint32_t symbol_id) const;

  void Reset();

 private:
  std::uint64_t MakeKey(std::uint32_t account_id,
                        std::uint32_t symbol_id) const;
  Status UpsertEntry(std::uint32_t account_id, std::uint32_t symbol_id,
                     std::int64_t delta_qty, std::int64_t delta_cost,
                     std::uint32_t trade_date);
  Status ValidateSlice(const ProRataSlice& slice) const;

  PositionLedgerConfig config_;
  std::vector<PositionEntry> entries_;
  std::unordered_map<std::uint64_t, std::size_t> index_;
};

}  // namespace desk
}  // namespace tkr
