#include "tkr/desk/order_blotter.h"

#include <algorithm>

namespace tkr {
namespace desk {

OrderBlotter::OrderBlotter(std::int64_t created_baseline_millis)
    : next_entry_id_(1), created_baseline_millis_(created_baseline_millis) {}

Status OrderBlotter::IngestBatch(const BatchWireFrame& frame) {
  const std::int64_t ts =
      created_baseline_millis_ + static_cast<std::int64_t>(frame.header.trade_date_yyyymmdd);
  for (const WireBatchRecord& rec : frame.records) {
    BlotterEntry entry{};
    entry.entry_id = next_entry_id_++;
    entry.batch_id = frame.header.desk_id;
    entry.record_id = rec.record_id;
    entry.account_id = rec.account_id;
    entry.symbol_id = rec.symbol_id;
    entry.qty_milli = rec.qty_milli;
    entry.price_tick = rec.price_tick;
    entry.notional_cents =
        (static_cast<std::int64_t>(rec.qty_milli) *
         static_cast<std::int64_t>(rec.price_tick)) /
        1000;
    entry.created_at_millis = ts + rec.record_id;
    entry.route_state = "OPEN";
    open_by_record_id_[rec.record_id] = entry;
  }
  return Status::kOk;
}

BlotterSnapshot OrderBlotter::Snapshot() const {
  BlotterSnapshot snap{};
  snap.status = Status::kOk;
  snap.entries.reserve(open_by_record_id_.size());
  for (const auto& entry : open_by_record_id_) {
    snap.entries.push_back(entry.second);
  }
  std::sort(snap.entries.begin(), snap.entries.end(),
            [](const BlotterEntry& a, const BlotterEntry& b) {
              return a.entry_id < b.entry_id;
            });
  for (const BlotterEntry& entry : snap.entries) {
    snap.total_notional_cents += entry.notional_cents;
    if (entry.route_state == "OPEN") {
      ++snap.open_orders;
    }
  }
  return snap;
}

Status OrderBlotter::MarkRouted(std::uint32_t record_id) {
  auto it = open_by_record_id_.find(record_id);
  if (it == open_by_record_id_.end()) {
    return Status::kBoundsError;
  }
  it->second.route_state = "ROUTED";
  return Status::kOk;
}

Status OrderBlotter::MarkFilled(std::uint32_t record_id,
                                std::uint32_t filled_qty_milli) {
  auto it = open_by_record_id_.find(record_id);
  if (it == open_by_record_id_.end()) {
    return Status::kBoundsError;
  }
  BlotterEntry& entry = it->second;
  if (filled_qty_milli <= 0 || filled_qty_milli > entry.qty_milli) {
    return Status::kBoundsError;
  }
  entry.qty_milli -= filled_qty_milli;
  entry.notional_cents =
      (static_cast<std::int64_t>(entry.qty_milli) *
       static_cast<std::int64_t>(entry.price_tick)) /
      1000;
  entry.route_state = entry.qty_milli == 0 ? "FILLED" : "PARTIAL";
  if (entry.qty_milli == 0) {
    open_by_record_id_.erase(it);
  }
  return Status::kOk;
}

std::vector<BlotterEntry> OrderBlotter::EntriesForAccount(
    std::uint32_t account_id) const {
  std::vector<BlotterEntry> out;
  for (const auto& entry : open_by_record_id_) {
    if (entry.second.account_id == account_id) {
      out.push_back(entry.second);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const BlotterEntry& a, const BlotterEntry& b) {
              return a.symbol_id < b.symbol_id;
            });
  return out;
}

std::int64_t OrderBlotter::OpenNotionalForSymbol(std::uint32_t symbol_id) const {
  std::int64_t total = 0;
  for (const auto& entry : open_by_record_id_) {
    if (entry.second.symbol_id == symbol_id && entry.second.route_state == "OPEN") {
      total += entry.second.notional_cents;
    }
  }
  return total;
}

void OrderBlotter::ClearFilled() {
  for (auto it = open_by_record_id_.begin(); it != open_by_record_id_.end();) {
    if (it->second.route_state == "FILLED") {
      it = open_by_record_id_.erase(it);
    } else {
      ++it;
    }
  }
}

std::int32_t OrderBlotter::OpenCount() const {
  return static_cast<std::int32_t>(open_by_record_id_.size());
}

}  // namespace desk
}  // namespace tkr
