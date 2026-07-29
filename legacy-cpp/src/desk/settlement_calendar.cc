#include "tkr/desk/settlement_calendar.h"

#include <algorithm>

namespace tkr {
namespace desk {
namespace {

constexpr std::uint8_t kSaturday = 6;
constexpr std::uint8_t kSunday = 0;

}  // namespace

SettlementCalendar::SettlementCalendar(SettlementCalendarConfig config)
    : config_(config) {
  LoadUsEquityHolidays(20260101 / 10000);
}

std::uint64_t SettlementCalendar::HolidayKey(std::uint32_t date,
                                             MarketRegion region) const {
  return (static_cast<std::uint64_t>(date) << 8) |
         static_cast<std::uint64_t>(region);
}

void SettlementCalendar::AddHoliday(const HolidayEntry& holiday) {
  holidays_.insert(HolidayKey(holiday.date_yyyymmdd, holiday.region));
  holiday_table_.push_back(holiday);
}

void SettlementCalendar::RemoveHoliday(std::uint32_t date_yyyymmdd,
                                       MarketRegion region) {
  holidays_.erase(HolidayKey(date_yyyymmdd, region));
  holiday_table_.erase(
      std::remove_if(holiday_table_.begin(), holiday_table_.end(),
                     [date_yyyymmdd, region](const HolidayEntry& h) {
                       return h.date_yyyymmdd == date_yyyymmdd &&
                              h.region == region;
                     }),
      holiday_table_.end());
}

void SettlementCalendar::LoadUsEquityHolidays(std::uint32_t year) {
  const struct {
    std::uint8_t month;
    std::uint8_t day;
    const char* name;
  } us_holidays[] = {
      {1, 1, "New Year's Day"},
      {1, 20, "MLK Day"},
      {2, 17, "Presidents Day"},
      {4, 18, "Good Friday"},
      {5, 26, "Memorial Day"},
      {6, 19, "Juneteenth"},
      {7, 4, "Independence Day"},
      {9, 1, "Labor Day"},
      {11, 27, "Thanksgiving"},
      {12, 25, "Christmas"},
  };

  for (const auto& h : us_holidays) {
    HolidayEntry entry{};
    entry.date_yyyymmdd = EncodeYyyymmdd(static_cast<std::uint16_t>(year),
                                         h.month, h.day);
    entry.region = MarketRegion::kUsEquity;
    entry.name = h.name;
    entry.early_close = false;
    AddHoliday(entry);
  }
}

void SettlementCalendar::LoadEuEquityHolidays(std::uint32_t year) {
  const struct {
    std::uint8_t month;
    std::uint8_t day;
    const char* name;
  } eu_holidays[] = {
      {1, 1, "New Year"},
      {5, 1, "Labour Day"},
      {12, 25, "Christmas"},
      {12, 26, "Boxing Day"},
  };

  for (const auto& h : eu_holidays) {
    HolidayEntry entry{};
    entry.date_yyyymmdd = EncodeYyyymmdd(static_cast<std::uint16_t>(year),
                                         h.month, h.day);
    entry.region = MarketRegion::kEuEquity;
    entry.name = h.name;
    entry.early_close = false;
    AddHoliday(entry);
  }
}

void SettlementCalendar::DecodeYyyymmdd(std::uint32_t date,
                                          std::uint16_t* year,
                                          std::uint8_t* month,
                                          std::uint8_t* day) {
  if (year != nullptr) {
    *year = static_cast<std::uint16_t>(date / 10000);
  }
  if (month != nullptr) {
    *month = static_cast<std::uint8_t>((date / 100) % 100);
  }
  if (day != nullptr) {
    *day = static_cast<std::uint8_t>(date % 100);
  }
}

std::uint32_t SettlementCalendar::EncodeYyyymmdd(std::uint16_t year,
                                                 std::uint8_t month,
                                                 std::uint8_t day) {
  return static_cast<std::uint32_t>(year) * 10000u +
         static_cast<std::uint32_t>(month) * 100u +
         static_cast<std::uint32_t>(day);
}

bool SettlementCalendar::IsLeapYear(std::uint16_t year) const {
  if (year % 400 == 0) {
    return true;
  }
  if (year % 100 == 0) {
    return false;
  }
  return year % 4 == 0;
}

std::uint8_t SettlementCalendar::DaysInMonth(std::uint16_t year,
                                              std::uint8_t month) const {
  static const std::uint8_t days[] = {0,  31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return days[month];
}

std::uint8_t SettlementCalendar::DayOfWeek(std::uint32_t date_yyyymmdd) const {
  std::uint16_t year = 0;
  std::uint8_t month = 0;
  std::uint8_t day = 0;
  DecodeYyyymmdd(date_yyyymmdd, &year, &month, &day);

  if (month < 3) {
    month = static_cast<std::uint8_t>(month + 12);
    year = static_cast<std::uint16_t>(year - 1);
  }

  const int k = year % 100;
  const int j = year / 100;
  const int h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return static_cast<std::uint8_t>((h + 6) % 7);
}

std::uint32_t SettlementCalendar::IncrementDate(std::uint32_t date) const {
  std::uint16_t year = 0;
  std::uint8_t month = 0;
  std::uint8_t day = 0;
  DecodeYyyymmdd(date, &year, &month, &day);

  day = static_cast<std::uint8_t>(day + 1);
  const std::uint8_t dim = DaysInMonth(year, month);
  if (day > dim) {
    day = 1;
    month = static_cast<std::uint8_t>(month + 1);
    if (month > 12) {
      month = 1;
      year = static_cast<std::uint16_t>(year + 1);
    }
  }
  return EncodeYyyymmdd(year, month, day);
}

std::uint32_t SettlementCalendar::DecrementDate(std::uint32_t date) const {
  std::uint16_t year = 0;
  std::uint8_t month = 0;
  std::uint8_t day = 0;
  DecodeYyyymmdd(date, &year, &month, &day);

  if (day <= 1) {
    month = static_cast<std::uint8_t>(month - 1);
    if (month < 1) {
      month = 12;
      year = static_cast<std::uint16_t>(year - 1);
    }
    day = DaysInMonth(year, month);
  } else {
    day = static_cast<std::uint8_t>(day - 1);
  }
  return EncodeYyyymmdd(year, month, day);
}

bool SettlementCalendar::IsWeekend(std::uint32_t date_yyyymmdd) const {
  const std::uint8_t dow = DayOfWeek(date_yyyymmdd);
  return dow == kSaturday || dow == kSunday;
}

bool SettlementCalendar::IsHoliday(std::uint32_t date_yyyymmdd,
                                   MarketRegion region) const {
  return holidays_.count(HolidayKey(date_yyyymmdd, region)) > 0;
}

bool SettlementCalendar::IsBusinessDay(std::uint32_t date_yyyymmdd) const {
  return IsBusinessDay(date_yyyymmdd, config_.default_region);
}

bool SettlementCalendar::IsBusinessDay(std::uint32_t date_yyyymmdd,
                                       MarketRegion region) const {
  if (config_.skip_weekends && IsWeekend(date_yyyymmdd)) {
    return false;
  }
  if (IsHoliday(date_yyyymmdd, region)) {
    return false;
  }
  return true;
}

std::uint32_t SettlementCalendar::NextBusinessDay(
    std::uint32_t date_yyyymmdd) const {
  std::uint32_t cursor = IncrementDate(date_yyyymmdd);
  std::uint32_t guard = 0;
  while (!IsBusinessDay(cursor) && guard < 366) {
    cursor = IncrementDate(cursor);
    ++guard;
  }
  return cursor;
}

std::uint32_t SettlementCalendar::PrevBusinessDay(
    std::uint32_t date_yyyymmdd) const {
  std::uint32_t cursor = DecrementDate(date_yyyymmdd);
  std::uint32_t guard = 0;
  while (!IsBusinessDay(cursor) && guard < 366) {
    cursor = DecrementDate(cursor);
    ++guard;
  }
  return cursor;
}

std::uint32_t SettlementCalendar::AddBusinessDays(std::uint32_t start_date,
                                                  std::uint32_t count) const {
  std::uint32_t cursor = start_date;
  for (std::uint32_t i = 0; i < count; ++i) {
    cursor = NextBusinessDay(cursor);
  }
  return cursor;
}

std::uint32_t SettlementCalendar::CycleToBusinessDays(SettlementCycle cycle) {
  switch (cycle) {
    case SettlementCycle::kT0:
      return 0;
    case SettlementCycle::kT1:
      return 1;
    case SettlementCycle::kT2:
      return 2;
    case SettlementCycle::kT3:
      return 3;
    case SettlementCycle::kT5:
      return 5;
    default:
      return 2;
  }
}

std::uint32_t SettlementCalendar::RollSettlementDate(
    std::uint32_t trade_date, SettlementCycle cycle) const {
  return RollSettlementDate(trade_date, cycle, config_.default_region);
}

std::uint32_t SettlementCalendar::RollSettlementDate(
    std::uint32_t trade_date, SettlementCycle cycle,
    MarketRegion region) const {
  const std::uint32_t biz_days = CycleToBusinessDays(cycle);
  std::uint32_t cursor = trade_date;
  std::uint32_t holidays_skipped = 0;

  for (std::uint32_t i = 0; i < biz_days; ++i) {
    cursor = NextBusinessDay(cursor);
    if (IsHoliday(cursor, region)) {
      ++holidays_skipped;
      cursor = NextBusinessDay(cursor);
    }
  }

  (void)holidays_skipped;
  return cursor;
}

SettlementCalendarResult SettlementCalendar::ComputeInstructions(
    const std::vector<WireBatchRecord>& records, std::uint32_t trade_date,
    SettlementCycle cycle) {
  SettlementCalendarResult result{};
  result.status = Status::kOk;
  result.instructions.reserve(records.size());

  for (const WireBatchRecord& rec : records) {
    SettlementInstruction instr{};
    instr.trade_date_yyyymmdd = trade_date;
    instr.settlement_date_yyyymmdd =
        RollSettlementDate(trade_date, cycle);
    instr.cycle = cycle;
    instr.region = config_.default_region;
    instr.record_id = rec.record_id;
    instr.account_id = rec.account_id;
    instr.qty_milli = rec.qty_milli;
    result.instructions.push_back(instr);
  }

  result.business_days_rolled = CycleToBusinessDays(cycle);
  return result;
}

SettlementCalendarResult SettlementCalendar::ComputeBatch(
    const BatchWireFrame& frame) {
  SettlementCycle cycle = config_.default_cycle;
  if ((frame.header.flags & kBatchFlagCrossDesk) != 0) {
    cycle = SettlementCycle::kT1;
  }
  return ComputeInstructions(frame.records, frame.header.trade_date_yyyymmdd,
                             cycle);
}

std::uint32_t SettlementCalendar::CountBusinessDaysBetween(
    std::uint32_t start_date, std::uint32_t end_date) const {
  if (end_date <= start_date) {
    return 0;
  }
  std::uint32_t count = 0;
  std::uint32_t cursor = start_date;
  std::uint32_t guard = 0;
  while (cursor < end_date && guard < 366 * 5) {
    cursor = IncrementDate(cursor);
    if (IsBusinessDay(cursor)) {
      ++count;
    }
    ++guard;
  }
  return count;
}

SettlementCalendarResult SettlementCalendar::RollBatchT2(
    const BatchWireFrame& frame) {
  SettlementCalendarResult result =
      ComputeBatch(frame);
  if (result.status != Status::kOk) {
    return result;
  }

  for (SettlementInstruction& instr : result.instructions) {
    instr.cycle = SettlementCycle::kT2;
    instr.settlement_date_yyyymmdd =
        RollSettlementDate(instr.trade_date_yyyymmdd, SettlementCycle::kT2);
    if (!IsBusinessDay(instr.settlement_date_yyyymmdd)) {
      instr.settlement_date_yyyymmdd =
          NextBusinessDay(instr.settlement_date_yyyymmdd);
      ++result.holidays_skipped;
    }
  }

  result.business_days_rolled = CycleToBusinessDays(SettlementCycle::kT2);
  return result;
}

bool SettlementCalendar::ValidateSettlementDate(
    std::uint32_t trade_date, std::uint32_t settle_date,
    SettlementCycle cycle) const {
  const std::uint32_t expected = RollSettlementDate(trade_date, cycle);
  return settle_date == expected;
}

std::vector<HolidayEntry> SettlementCalendar::HolidaysForRegion(
    MarketRegion region) const {
  std::vector<HolidayEntry> result;
  for (const HolidayEntry& entry : holiday_table_) {
    if (entry.region == region) {
      result.push_back(entry);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const HolidayEntry& a, const HolidayEntry& b) {
              return a.date_yyyymmdd < b.date_yyyymmdd;
            });
  return result;
}

std::uint32_t SettlementCalendar::AdjustForEarlyClose(
    std::uint32_t date_yyyymmdd, MarketRegion region) const {
  for (const HolidayEntry& entry : holiday_table_) {
    if (entry.date_yyyymmdd == date_yyyymmdd && entry.region == region &&
        entry.early_close) {
      return PrevBusinessDay(date_yyyymmdd);
    }
  }
  return date_yyyymmdd;
}

SettlementCalendarResult SettlementCalendar::ComputeWithCycleOverride(
    const BatchWireFrame& frame, SettlementCycle cycle,
    MarketRegion region) {
  SettlementCalendarResult result{};
  result.status = Status::kOk;
  result.instructions.reserve(frame.records.size());

  for (const WireBatchRecord& rec : frame.records) {
    SettlementInstruction instr{};
    instr.trade_date_yyyymmdd = frame.header.trade_date_yyyymmdd;
    instr.cycle = cycle;
    instr.region = region;
    instr.record_id = rec.record_id;
    instr.account_id = rec.account_id;
    instr.qty_milli = rec.qty_milli;

    std::uint32_t settle =
        RollSettlementDate(instr.trade_date_yyyymmdd, cycle, region);
    settle = AdjustForEarlyClose(settle, region);
    if (!IsBusinessDay(settle, region)) {
      settle = NextBusinessDay(settle);
      ++result.holidays_skipped;
    }
    instr.settlement_date_yyyymmdd = settle;
    result.instructions.push_back(instr);
  }

  result.business_days_rolled = CycleToBusinessDays(cycle);
  return result;
}

bool SettlementCalendar::IsEarlyClose(std::uint32_t date_yyyymmdd,
                                      MarketRegion region) const {
  for (const HolidayEntry& entry : holiday_table_) {
    if (entry.date_yyyymmdd == date_yyyymmdd && entry.region == region &&
        entry.early_close) {
      return true;
    }
  }
  return false;
}

}  // namespace desk
}  // namespace tkr
