#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tkr {
namespace wire {

enum class Mt940RecordKind : std::uint8_t {
  kUnknown = 0,
  kOpeningBalance,
  kClosingBalance,
  kStatementLine,
  kInfoLine,
};

struct Mt940Balance {
  char debit_credit;
  std::string date_yymmdd;
  std::string currency;
  std::int64_t amount_minor;
};

struct Mt940StatementLine {
  std::string value_date;
  std::string entry_date;
  char debit_credit;
  std::int64_t amount_minor;
  char funds_code;
  std::string transaction_type;
  std::string customer_reference;
  std::string bank_reference;
  std::string supplementary;
};

struct Mt940InfoSegment {
  std::string raw_code;
  std::string narrative;
  std::vector<std::string> subfields;
};

struct Mt940Statement {
  std::string transaction_reference;
  std::string account_id;
  std::string statement_number;
  Mt940Balance opening;
  Mt940Balance closing;
  std::vector<Mt940StatementLine> lines;
  std::vector<Mt940InfoSegment> info_segments;
};

struct Mt940ScanResult {
  Status status;
  Mt940Statement statement;
  std::size_t consumed_bytes;
  std::uint32_t line_count;
};

class SwiftMt940Scanner {
 public:
  SwiftMt940Scanner();

  Mt940ScanResult Scan(std::string_view swift_text);
  Status ScanField61(std::string_view field_body, Mt940StatementLine* out);
  Status ScanField86(std::string_view field_body, Mt940InfoSegment* out);

  static Status ParseBalance(std::string_view body, Mt940Balance* out);
  static bool IsDebitCredit(char dc);

 private:
  Status ScanBlock4(std::string_view block4, Mt940Statement* out);
  Status ScanTaggedField(std::string_view tag, std::string_view value,
                         Mt940Statement* out);
  Status ParseAmountMinor(std::string_view amount_text, char debit_credit,
                          std::int64_t* out);
  Status SplitSubfields86(std::string_view body,
                          std::vector<std::string>* subfields);

  std::vector<char> scratch_;
  std::uint32_t fields_seen_;
};

}  // namespace wire
}  // namespace tkr
