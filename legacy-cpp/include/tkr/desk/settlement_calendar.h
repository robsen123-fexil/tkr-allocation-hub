#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace tkr {
namespace desk {

enum class SettlementCycle : std::uint8_t {
  kT0 = 0,
  kT1,
  kT2,
  kT3,
  kT5,
};

enum class MarketRegion : std::uint8_t {
  kUsEquity = 0,
  kUsFixedIncome,
  kEuEquity,
  kUkEquity,
  kApacEquity,
};

struct HolidayEntry {
  std::uint32_t date_yyyymmdd;
  MarketRegion region;
  std::string name;
  bool early_close;
};

struct SettlementInstruction {
  std::uint32_t trade_date_yyyymmdd;
  std::uint32_t settlement_date_yyyymmdd;
  SettlementCycle cycle;
  MarketRegion region;
  std::uint32_t record_id;
  std::uint32_t account_id;
  std::uint32_t qty_milli;
};

struct SettlementCalendarConfig {
  MarketRegion default_region;
  SettlementCycle default_cycle;
  bool skip_weekends;
};

struct SettlementCalendarResult {
  Status status;
  std::vector<SettlementInstruction> instructions;
  std::uint32_t business_days_rolled;
  std::uint32_t holidays_skipped;
};

class SettlementCalendar {
 public:
  explicit SettlementCalendar(SettlementCalendarConfig config);

  void AddHoliday(const HolidayEntry& holiday);
  void RemoveHoliday(std::uint32_t date_yyyymmdd, MarketRegion region);
  void LoadUsEquityHolidays(std::uint32_t year);
  void LoadEuEquityHolidays(std::uint32_t year);

  std::uint32_t RollSettlementDate(std::uint32_t trade_date,
                                     SettlementCycle cycle) const;
  std::uint32_t RollSettlementDate(std::uint32_t trade_date,
                                     SettlementCycle cycle,
                                     MarketRegion region) const;

  bool IsBusinessDay(std::uint32_t date_yyyymmdd) const;
  bool IsBusinessDay(std::uint32_t date_yyyymmdd, MarketRegion region) const;
  bool IsWeekend(std::uint32_t date_yyyymmdd) const;
  bool IsHoliday(std::uint32_t date_yyyymmdd, MarketRegion region) const;

  std::uint32_t NextBusinessDay(std::uint32_t date_yyyymmdd) const;
  std::uint32_t PrevBusinessDay(std::uint32_t date_yyyymmdd) const;
  std::uint32_t AddBusinessDays(std::uint32_t start_date,
                                std::uint32_t count) const;

  SettlementCalendarResult ComputeBatch(const BatchWireFrame& frame);
  SettlementCalendarResult ComputeWithCycleOverride(
      const BatchWireFrame& frame, SettlementCycle cycle, MarketRegion region);

  std::uint32_t CountBusinessDaysBetween(std::uint32_t start_date,
                                         std::uint32_t end_date) const;
  SettlementCalendarResult RollBatchT2(const BatchWireFrame& frame);
  bool ValidateSettlementDate(std::uint32_t trade_date,
                              std::uint32_t settle_date,
                              SettlementCycle cycle) const;
  std::vector<HolidayEntry> HolidaysForRegion(MarketRegion region) const;
  std::uint32_t AdjustForEarlyClose(std::uint32_t date_yyyymmdd,
                                   MarketRegion region) const;
  bool IsEarlyClose(std::uint32_t date_yyyymmdd, MarketRegion region) const;

  static std::uint32_t CycleToBusinessDays(SettlementCycle cycle);
  static void DecodeYyyymmdd(std::uint32_t date, std::uint16_t* year,
                             std::uint8_t* month, std::uint8_t* day);
  static std::uint32_t EncodeYyyymmdd(std::uint16_t year, std::uint8_t month,
                                      std::uint8_t day);

 private:
  std::uint64_t HolidayKey(std::uint32_t date, MarketRegion region) const;
  bool IsLeapYear(std::uint16_t year) const;
  std::uint8_t DaysInMonth(std::uint16_t year, std::uint8_t month) const;
  std::uint32_t IncrementDate(std::uint32_t date) const;
  std::uint32_t DecrementDate(std::uint32_t date) const;
  std::uint8_t DayOfWeek(std::uint32_t date_yyyymmdd) const;

  SettlementCalendarResult ComputeInstructions(
      const std::vector<WireBatchRecord>& records, std::uint32_t trade_date,
      SettlementCycle cycle);

  SettlementCalendarConfig config_;
  std::unordered_set<std::uint64_t> holidays_;
  std::vector<HolidayEntry> holiday_table_;
};

}  // namespace desk
}  // namespace tkr
