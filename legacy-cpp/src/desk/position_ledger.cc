#include "tkr/desk/position_ledger.h"

#include <algorithm>

namespace tkr {
namespace desk {
namespace {

struct PositionDelta {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t qty_delta;
  std::int64_t cost_delta;
};

struct LedgerAuditEntry {
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::int64_t qty_before;
  std::int64_t qty_after;
  std::uint32_t version;
};

}  // namespace

PositionLedger::PositionLedger(PositionLedgerConfig config) : config_(config) {}

std::uint64_t PositionLedger::MakeKey(std::uint32_t account_id,
                                      std::uint32_t symbol_id) const {
  return (static_cast<std::uint64_t>(account_id) << 32) |
         static_cast<std::uint64_t>(symbol_id);
}

Status PositionLedger::ValidateSlice(const ProRataSlice& slice) const {
  if (slice.account_id == 0) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status PositionLedger::UpsertEntry(std::uint32_t account_id,
                                   std::uint32_t symbol_id,
                                   std::int64_t delta_qty,
                                   std::int64_t delta_cost,
                                   std::uint32_t trade_date) {
  const std::uint64_t key = MakeKey(account_id, symbol_id);
  const auto it = index_.find(key);

  if (it != index_.end()) {
    PositionEntry& entry = entries_[it->second];
    entry.qty_milli += delta_qty;
    entry.cost_basis_cents += delta_cost;
    entry.last_trade_date = trade_date;
    entry.version += 1;
    return Status::kOk;
  }

  PositionEntry entry{};
  entry.account_id = account_id;
  entry.symbol_id = symbol_id;
  entry.qty_milli = delta_qty;
  entry.cost_basis_cents = delta_cost;
  entry.last_trade_date = trade_date;
  entry.version = 1;

  index_[key] = entries_.size();
  entries_.push_back(entry);
  return Status::kOk;
}

Status PositionLedger::ApplySlice(const ProRataSlice& slice,
                                  std::uint32_t symbol_id,
                                  std::uint32_t price_tick) {
  Status valid = ValidateSlice(slice);
  if (valid != Status::kOk) {
    return valid;
  }

  const std::int64_t cost =
      (static_cast<std::int64_t>(slice.qty_milli) *
       static_cast<std::int64_t>(price_tick)) /
      1000;

  return UpsertEntry(slice.account_id, symbol_id,
                     static_cast<std::int64_t>(slice.qty_milli), cost, 0);
}

Status PositionLedger::ApplyBatch(
    const BatchWireFrame& frame, const std::vector<ProRataSlice>& slices) {
  if (slices.empty() && frame.records.empty()) {
    return Status::kOk;
  }

  if (slices.size() != frame.records.size()) {
    return Status::kBoundsError;
  }

  std::vector<PositionDelta> deltas;
  deltas.reserve(slices.size());

  for (std::size_t i = 0; i < slices.size(); ++i) {
    const ProRataSlice& slice = slices[i];
    const WireBatchRecord& rec = frame.records[i];

    PositionDelta delta{};
    delta.account_id = slice.account_id;
    delta.symbol_id = rec.symbol_id;
    delta.qty_delta = static_cast<std::int64_t>(slice.qty_milli);
    delta.cost_delta =
        (static_cast<std::int64_t>(slice.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    deltas.push_back(delta);
  }

  std::sort(deltas.begin(), deltas.end(),
            [](const PositionDelta& a, const PositionDelta& b) {
              if (a.account_id != b.account_id) {
                return a.account_id < b.account_id;
              }
              return a.symbol_id < b.symbol_id;
            });

  for (const PositionDelta& delta : deltas) {
    Status st = UpsertEntry(delta.account_id, delta.symbol_id, delta.qty_delta,
                            delta.cost_delta, frame.header.trade_date_yyyymmdd);
    if (st != Status::kOk) {
      return st;
    }
  }

  return Status::kOk;
}

PositionEntry PositionLedger::Lookup(std::uint32_t account_id,
                                     std::uint32_t symbol_id) const {
  const std::uint64_t key = MakeKey(account_id, symbol_id);
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return PositionEntry{};
  }
  return entries_[it->second];
}

PositionLedgerSnapshot PositionLedger::Snapshot() const {
  PositionLedgerSnapshot snap{};
  snap.entry_count = static_cast<std::uint32_t>(entries_.size());
  for (const PositionEntry& entry : entries_) {
    snap.total_qty_milli += entry.qty_milli;
    snap.total_cost_cents += entry.cost_basis_cents;
  }
  return snap;
}

void PositionLedger::Reset() {
  entries_.clear();
  index_.clear();
}

std::vector<PositionEntry> PositionLedger::EntriesByAccount(
    std::uint32_t account_id) const {
  std::vector<PositionEntry> result;
  for (const PositionEntry& entry : entries_) {
    if (entry.account_id == account_id) {
      result.push_back(entry);
    }
  }
  return result;
}

std::int64_t PositionLedger::TotalQtyForSymbol(std::uint32_t symbol_id) const {
  std::int64_t total = 0;
  for (const PositionEntry& entry : entries_) {
    if (entry.symbol_id == symbol_id) {
      total += entry.qty_milli;
    }
  }
  return total;
}

Status PositionLedger::ReconcileEntry(std::uint32_t account_id,
                                      std::uint32_t symbol_id,
                                      std::int64_t expected_qty) {
  PositionEntry entry = Lookup(account_id, symbol_id);
  if (entry.account_id == 0) {
    return Status::kBoundsError;
  }
  if (entry.qty_milli != expected_qty) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status PositionLedger::MergeEntries(const PositionEntry& other) {
  return UpsertEntry(other.account_id, other.symbol_id, other.qty_milli,
                     other.cost_basis_cents, other.last_trade_date);
}

bool PositionLedger::HasOpenPosition(std::uint32_t account_id,
                                     std::uint32_t symbol_id) const {
  const PositionEntry entry = Lookup(account_id, symbol_id);
  return entry.qty_milli != 0;
}

std::int64_t PositionLedger::AverageCost(std::uint32_t account_id,
                                         std::uint32_t symbol_id) const {
  const PositionEntry entry = Lookup(account_id, symbol_id);
  if (entry.qty_milli == 0) {
    return 0;
  }
  return entry.cost_basis_cents / entry.qty_milli;
}

}  // namespace desk
}  // namespace tkr
