package com.tkr.desk;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.types.WireTypes.Status;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * T+n settlement calendar with US/EU holidays and business-day rolling.
 * Ported from legacy-cpp settlement_calendar.cc.
 */
public final class SettlementCalendar {

    public enum MarketRegion { US_EQUITY, EU_EQUITY, UK_EQUITY, APAC_EQUITY }

    public enum SettlementCycle { T0, T1, T2, T3, T5 }

    public static final class HolidayEntry {
        public int dateYyyymmdd;
        public MarketRegion region;
        public String name = "";
        public boolean earlyClose;
    }

    public static final class SettlementInstruction {
        public int tradeDateYyyymmdd;
        public int settlementDateYyyymmdd;
        public SettlementCycle cycle;
        public MarketRegion region;
        public int recordId;
        public int accountId;
        public int qtyMilli;
    }

    public static final class SettlementCalendarConfig {
        public MarketRegion defaultRegion = MarketRegion.US_EQUITY;
        public SettlementCycle defaultCycle = SettlementCycle.T2;
        public boolean skipWeekends = true;

        public SettlementCalendarConfig(MarketRegion region, SettlementCycle cycle, boolean skipWeekends) {
            this.defaultRegion = region;
            this.defaultCycle = cycle;
            this.skipWeekends = skipWeekends;
        }
    }

    public static final class SettlementCalendarResult {
        public Status status = Status.OK;
        public List<SettlementInstruction> instructions = new ArrayList<>();
        public int businessDaysRolled;
        public int holidaysSkipped;
    }

    private static final int SATURDAY = 6;
    private static final int SUNDAY = 0;

    private final SettlementCalendarConfig config;
    private final Set<Long> holidays = new HashSet<>();
    private final List<HolidayEntry> holidayTable = new ArrayList<>();

    public SettlementCalendar() {
        this(new SettlementCalendarConfig(MarketRegion.US_EQUITY, SettlementCycle.T2, true));
    }

    public SettlementCalendar(SettlementCalendarConfig config) {
        this.config = config != null ? config : new SettlementCalendarConfig(MarketRegion.US_EQUITY, SettlementCycle.T2, true);
        loadUsEquityHolidays(2026);
    }

    private long holidayKey(int date, MarketRegion region) {
        return ((long) date << 8) | region.ordinal();
    }

    public void addHoliday(HolidayEntry holiday) {
        holidays.add(holidayKey(holiday.dateYyyymmdd, holiday.region));
        holidayTable.add(holiday);
    }

    public void removeHoliday(int dateYyyymmdd, MarketRegion region) {
        holidays.remove(holidayKey(dateYyyymmdd, region));
        holidayTable.removeIf(h -> h.dateYyyymmdd == dateYyyymmdd && h.region == region);
    }

    public void loadUsEquityHolidays(int year) {
        int[][] usHolidays = {
            {1, 1}, {1, 20}, {2, 17}, {4, 18}, {5, 26},
            {6, 19}, {7, 4}, {9, 1}, {11, 27}, {12, 25}
        };
        String[] names = {
            "New Year's Day", "MLK Day", "Presidents Day", "Good Friday", "Memorial Day",
            "Juneteenth", "Independence Day", "Labor Day", "Thanksgiving", "Christmas"
        };
        for (int i = 0; i < usHolidays.length; i++) {
            HolidayEntry entry = new HolidayEntry();
            entry.dateYyyymmdd = encodeYyyymmdd(year, usHolidays[i][0], usHolidays[i][1]);
            entry.region = MarketRegion.US_EQUITY;
            entry.name = names[i];
            addHoliday(entry);
        }
    }

    public void loadEuEquityHolidays(int year) {
        int[][] euHolidays = {{1, 1}, {5, 1}, {12, 25}, {12, 26}};
        String[] names = {"New Year", "Labour Day", "Christmas", "Boxing Day"};
        for (int i = 0; i < euHolidays.length; i++) {
            HolidayEntry entry = new HolidayEntry();
            entry.dateYyyymmdd = encodeYyyymmdd(year, euHolidays[i][0], euHolidays[i][1]);
            entry.region = MarketRegion.EU_EQUITY;
            entry.name = names[i];
            addHoliday(entry);
        }
    }

    public static void decodeYyyymmdd(int date, int[] out) {
        out[0] = date / 10000;
        out[1] = (date / 100) % 100;
        out[2] = date % 100;
    }

    public static int encodeYyyymmdd(int year, int month, int day) {
        return year * 10000 + month * 100 + day;
    }

    public boolean isLeapYear(int year) {
        if (year % 400 == 0) return true;
        if (year % 100 == 0) return false;
        return year % 4 == 0;
    }

    public int daysInMonth(int year, int month) {
        int[] days = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month < 1 || month > 12) return 0;
        if (month == 2 && isLeapYear(year)) return 29;
        return days[month];
    }

    public int dayOfWeek(int dateYyyymmdd) {
        int[] parts = new int[3];
        decodeYyyymmdd(dateYyyymmdd, parts);
        int year = parts[0];
        int month = parts[1];
        int day = parts[2];
        if (month < 3) {
            month += 12;
            year -= 1;
        }
        int k = year % 100;
        int j = year / 100;
        int h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
        return (h + 6) % 7;
    }

    public int incrementDate(int date) {
        int[] parts = new int[3];
        decodeYyyymmdd(date, parts);
        int day = parts[2] + 1;
        int month = parts[1];
        int year = parts[0];
        if (day > daysInMonth(year, month)) {
            day = 1;
            month++;
            if (month > 12) {
                month = 1;
                year++;
            }
        }
        return encodeYyyymmdd(year, month, day);
    }

    public int decrementDate(int date) {
        int[] parts = new int[3];
        decodeYyyymmdd(date, parts);
        int day = parts[2];
        int month = parts[1];
        int year = parts[0];
        if (day <= 1) {
            month--;
            if (month < 1) {
                month = 12;
                year--;
            }
            day = daysInMonth(year, month);
        } else {
            day--;
        }
        return encodeYyyymmdd(year, month, day);
    }

    public boolean isWeekend(int dateYyyymmdd) {
        int dow = dayOfWeek(dateYyyymmdd);
        return dow == SATURDAY || dow == SUNDAY;
    }

    public boolean isHoliday(int dateYyyymmdd, MarketRegion region) {
        return holidays.contains(holidayKey(dateYyyymmdd, region));
    }

    public boolean isBusinessDay(int dateYyyymmdd) {
        return isBusinessDay(dateYyyymmdd, config.defaultRegion);
    }

    public boolean isBusinessDay(int dateYyyymmdd, MarketRegion region) {
        if (config.skipWeekends && isWeekend(dateYyyymmdd)) return false;
        return !isHoliday(dateYyyymmdd, region);
    }

    public int nextBusinessDay(int dateYyyymmdd) {
        int cursor = incrementDate(dateYyyymmdd);
        int guard = 0;
        while (!isBusinessDay(cursor) && guard < 366) {
            cursor = incrementDate(cursor);
            guard++;
        }
        return cursor;
    }

    public int prevBusinessDay(int dateYyyymmdd) {
        int cursor = decrementDate(dateYyyymmdd);
        int guard = 0;
        while (!isBusinessDay(cursor) && guard < 366) {
            cursor = decrementDate(cursor);
            guard++;
        }
        return cursor;
    }

    public int addBusinessDays(int startDate, int count) {
        int cursor = startDate;
        for (int i = 0; i < count; i++) cursor = nextBusinessDay(cursor);
        return cursor;
    }

    public static int cycleToBusinessDays(SettlementCycle cycle) {
        return switch (cycle) {
            case T0 -> 0;
            case T1 -> 1;
            case T2 -> 2;
            case T3 -> 3;
            case T5 -> 5;
        };
    }

    public int rollSettlementDate(int tradeDate, SettlementCycle cycle) {
        return rollSettlementDate(tradeDate, cycle, config.defaultRegion);
    }

    public int rollSettlementDate(int tradeDate, SettlementCycle cycle, MarketRegion region) {
        int bizDays = cycleToBusinessDays(cycle);
        int cursor = tradeDate;
        for (int i = 0; i < bizDays; i++) {
            cursor = nextBusinessDay(cursor);
            if (isHoliday(cursor, region)) cursor = nextBusinessDay(cursor);
        }
        return cursor;
    }

    public SettlementCalendarResult computeInstructions(List<WireBatchRecord> records, int tradeDate, SettlementCycle cycle) {
        SettlementCalendarResult result = new SettlementCalendarResult();
        result.status = Status.OK;
        for (WireBatchRecord rec : records) {
            SettlementInstruction instr = new SettlementInstruction();
            instr.tradeDateYyyymmdd = tradeDate;
            instr.settlementDateYyyymmdd = rollSettlementDate(tradeDate, cycle);
            instr.cycle = cycle;
            instr.region = config.defaultRegion;
            instr.recordId = rec.recordId;
            instr.accountId = rec.accountId;
            instr.qtyMilli = rec.qtyMilli;
            result.instructions.add(instr);
        }
        result.businessDaysRolled = cycleToBusinessDays(cycle);
        return result;
    }

    public SettlementCalendarResult computeBatch(BatchWireFrame frame) {
        SettlementCycle cycle = config.defaultCycle;
        if ((frame.header.flags & WireTypes.BATCH_FLAG_CROSS_DESK) != 0) cycle = SettlementCycle.T1;
        return computeInstructions(frame.records, frame.header.tradeDateYyyymmdd, cycle);
    }

    public int countBusinessDaysBetween(int startDate, int endDate) {
        if (endDate <= startDate) return 0;
        int count = 0;
        int cursor = startDate;
        int guard = 0;
        while (cursor < endDate && guard < 366 * 5) {
            cursor = incrementDate(cursor);
            if (isBusinessDay(cursor)) count++;
            guard++;
        }
        return count;
    }

    public SettlementCalendarResult rollBatchT2(BatchWireFrame frame) {
        SettlementCalendarResult result = computeBatch(frame);
        if (result.status != Status.OK) return result;
        for (SettlementInstruction instr : result.instructions) {
            instr.cycle = SettlementCycle.T2;
            instr.settlementDateYyyymmdd = rollSettlementDate(instr.tradeDateYyyymmdd, SettlementCycle.T2);
            if (!isBusinessDay(instr.settlementDateYyyymmdd)) {
                instr.settlementDateYyyymmdd = nextBusinessDay(instr.settlementDateYyyymmdd);
                result.holidaysSkipped++;
            }
        }
        result.businessDaysRolled = cycleToBusinessDays(SettlementCycle.T2);
        return result;
    }

    public boolean validateSettlementDate(int tradeDate, int settleDate, SettlementCycle cycle) {
        return settleDate == rollSettlementDate(tradeDate, cycle);
    }

    public List<HolidayEntry> holidaysForRegion(MarketRegion region) {
        List<HolidayEntry> result = new ArrayList<>();
        for (HolidayEntry entry : holidayTable) {
            if (entry.region == region) result.add(entry);
        }
        result.sort(Comparator.comparingInt(h -> h.dateYyyymmdd));
        return result;
    }

    public int adjustForEarlyClose(int dateYyyymmdd, MarketRegion region) {
        for (HolidayEntry entry : holidayTable) {
            if (entry.dateYyyymmdd == dateYyyymmdd && entry.region == region && entry.earlyClose) {
                return prevBusinessDay(dateYyyymmdd);
            }
        }
        return dateYyyymmdd;
    }

    public SettlementCalendarResult computeWithCycleOverride(BatchWireFrame frame, SettlementCycle cycle, MarketRegion region) {
        SettlementCalendarResult result = new SettlementCalendarResult();
        result.status = Status.OK;
        for (WireBatchRecord rec : frame.records) {
            SettlementInstruction instr = new SettlementInstruction();
            instr.tradeDateYyyymmdd = frame.header.tradeDateYyyymmdd;
            instr.cycle = cycle;
            instr.region = region;
            instr.recordId = rec.recordId;
            instr.accountId = rec.accountId;
            instr.qtyMilli = rec.qtyMilli;
            int settle = rollSettlementDate(instr.tradeDateYyyymmdd, cycle, region);
            settle = adjustForEarlyClose(settle, region);
            if (!isBusinessDay(settle, region)) {
                settle = nextBusinessDay(settle);
                result.holidaysSkipped++;
            }
            instr.settlementDateYyyymmdd = settle;
            result.instructions.add(instr);
        }
        result.businessDaysRolled = cycleToBusinessDays(cycle);
        return result;
    }

    public boolean isEarlyClose(int dateYyyymmdd, MarketRegion region) {
        for (HolidayEntry entry : holidayTable) {
            if (entry.dateYyyymmdd == dateYyyymmdd && entry.region == region && entry.earlyClose) return true;
        }
        return false;
    }

    public int businessDaysUntil(int fromDate, int toDate) {
        return countBusinessDaysBetween(fromDate, toDate);
    }

    public List<SettlementInstruction> instructionsForAccount(List<SettlementInstruction> instructions, int accountId) {
        List<SettlementInstruction> filtered = new ArrayList<>();
        for (SettlementInstruction instr : instructions) {
            if (instr.accountId == accountId) filtered.add(instr);
        }
        return filtered;
    }

    public SettlementCycle inferCycleFromFlags(int batchFlags) {
        if ((batchFlags & WireTypes.BATCH_FLAG_CROSS_DESK) != 0) return SettlementCycle.T1;
        return config.defaultCycle;
    }

    public int nearestSettlementDate(int tradeDate, SettlementCycle cycle, MarketRegion region) {
        int settle = rollSettlementDate(tradeDate, cycle, region);
        while (!isBusinessDay(settle, region)) settle = nextBusinessDay(settle);
        return settle;
    }

    public Map<String, Integer> holidayCountByRegion() {
        Map<String, Integer> counts = new HashMap<>();
        for (HolidayEntry entry : holidayTable) {
            String key = entry.region.name();
            counts.merge(key, 1, Integer::sum);
        }
        return counts;
    }

    public boolean spansWeekend(int startDate, int endDate) {
        int cursor = startDate;
        while (cursor <= endDate) {
            if (isWeekend(cursor)) return true;
            cursor = incrementDate(cursor);
        }
        return false;
    }

    public String formatInstruction(SettlementInstruction instr) {
        return "rec=" + instr.recordId + " trade=" + instr.tradeDateYyyymmdd
                + " settle=" + instr.settlementDateYyyymmdd + " cycle=" + instr.cycle;
    }
}
