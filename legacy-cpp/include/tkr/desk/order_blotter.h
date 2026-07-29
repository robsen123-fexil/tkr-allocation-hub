#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tkr {
namespace desk {

struct BlotterEntry {
  std::uint32_t entry_id;
  std::uint32_t batch_id;
  std::uint32_t record_id;
  std::uint32_t account_id;
  std::uint32_t symbol_id;
  std::uint32_t qty_milli;
  std::uint32_t price_tick;
  std::int64_t notional_cents;
  std::int64_t created_at_millis;
  std::string route_state;
};

struct BlotterSnapshot {
  Status status;
  std::vector<BlotterEntry> entries;
  std::int64_t total_notional_cents;
  std::int32_t open_orders;
};

class OrderBlotter {
 public:
  explicit OrderBlotter(std::int64_t created_baseline_millis);

  Status IngestBatch(const BatchWireFrame& frame);
  BlotterSnapshot Snapshot() const;
  Status MarkRouted(std::uint32_t record_id);
  Status MarkFilled(std::uint32_t record_id, std::uint32_t filled_qty_milli);
  std::vector<BlotterEntry> EntriesForAccount(std::uint32_t account_id) const;
  std::int64_t OpenNotionalForSymbol(std::uint32_t symbol_id) const;
  void ClearFilled();
  std::int32_t OpenCount() const;

 private:
  std::map<std::uint32_t, BlotterEntry> open_by_record_id_;
  std::uint32_t next_entry_id_;
  std::int64_t created_baseline_millis_;
};

}  // namespace desk
}  // namespace tkr
