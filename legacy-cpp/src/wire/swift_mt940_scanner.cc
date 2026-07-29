#include "tkr/wire/swift_mt940_scanner.h"

#include "tkr/util/bounds.h"

#include <algorithm>
#include <cctype>

namespace tkr {
namespace wire {
namespace {

bool IsSwiftDigit(char ch) {
  return ch >= '0' && ch <= '9';
}

bool IsSwiftAlpha(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

std::string_view TrimSwift(std::string_view text) {
  while (!text.empty() && (text.front() == '\r' || text.front() == '\n' ||
                           text.front() == ' ')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                           text.back() == ' ')) {
    text.remove_suffix(1);
  }
  return text;
}

Status ParseYYMMDD(std::string_view text, std::string* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  if (text.size() != 6) {
    return Status::kBoundsError;
  }
  for (char ch : text) {
    if (!IsSwiftDigit(ch)) {
      return Status::kBoundsError;
    }
  }
  out->assign(text.data(), text.size());
  return Status::kOk;
}

}  // namespace

SwiftMt940Scanner::SwiftMt940Scanner() : fields_seen_(0) {}

bool SwiftMt940Scanner::IsDebitCredit(char dc) {
  return dc == 'C' || dc == 'D' || dc == 'R' || dc == 'c' || dc == 'd';
}

Status SwiftMt940Scanner::ParseBalance(std::string_view body, Mt940Balance* out) {
  if (out == nullptr || body.size() < 10) {
    return Status::kBoundsError;
  }
  out->debit_credit = body[0];
  if (!IsDebitCredit(out->debit_credit)) {
    return Status::kBoundsError;
  }
  Status date_status = ParseYYMMDD(body.substr(1, 6), &out->date_yymmdd);
  if (date_status != Status::kOk) {
    return date_status;
  }
  const std::size_t curr_start = 7;
  if (!util::SliceInBounds(curr_start, 3, body.size())) {
    return Status::kBoundsError;
  }
  out->currency.assign(body.substr(curr_start, 3));
  std::string_view amount_text = body.substr(curr_start + 3);
  return SwiftMt940Scanner{}.ParseAmountMinor(amount_text, out->debit_credit,
                                            &out->amount_minor);
}

Status SwiftMt940Scanner::ParseAmountMinor(std::string_view amount_text,
                                           char debit_credit,
                                           std::int64_t* out) {
  if (out == nullptr || amount_text.empty()) {
    return Status::kBoundsError;
  }
  std::int64_t whole = 0;
  std::int64_t frac = 0;
  bool seen_comma = false;
  std::size_t frac_digits = 0;
  for (char ch : amount_text) {
    if (ch == ',') {
      seen_comma = true;
      continue;
    }
    if (!IsSwiftDigit(ch)) {
      break;
    }
    const std::int64_t digit = static_cast<std::int64_t>(ch - '0');
    if (!seen_comma) {
      whole = whole * 10 + digit;
    } else if (frac_digits < 2) {
      frac = frac * 10 + digit;
      ++frac_digits;
    }
  }
  while (frac_digits < 2) {
    frac *= 10;
    ++frac_digits;
  }
  std::int64_t minor = whole * 100 + frac;
  if (debit_credit == 'D' || debit_credit == 'd') {
    minor = -minor;
  }
  *out = minor;
  return Status::kOk;
}

Status SwiftMt940Scanner::ScanField61(std::string_view field_body,
                                      Mt940StatementLine* out) {
  if (out == nullptr || field_body.size() < 12) {
    return Status::kBoundsError;
  }
  out->value_date.clear();
  out->entry_date.clear();
  out->supplementary.clear();
  out->transaction_type.clear();
  out->customer_reference.clear();
  out->bank_reference.clear();

  std::size_t cursor = 0;
  Status vd = ParseYYMMDD(field_body.substr(cursor, 6), &out->value_date);
  if (vd != Status::kOk) {
    return vd;
  }
  cursor += 6;

  if (cursor + 4 <= field_body.size() && IsSwiftDigit(field_body[cursor])) {
    Status ed = ParseYYMMDD(field_body.substr(cursor, 4), &out->entry_date);
    if (ed == Status::kOk) {
      cursor += 4;
    }
  }

  if (cursor >= field_body.size()) {
    return Status::kTruncated;
  }
  out->debit_credit = field_body[cursor++];
  if (!IsDebitCredit(out->debit_credit)) {
    return Status::kBoundsError;
  }

  if (cursor < field_body.size() &&
      (field_body[cursor] == 'C' || field_body[cursor] == 'D' ||
       field_body[cursor] == 'R' || field_body[cursor] == 'S')) {
    out->funds_code = field_body[cursor++];
  } else {
    out->funds_code = 'N';
  }

  const std::size_t amount_start = cursor;
  while (cursor < field_body.size() &&
         (IsSwiftDigit(field_body[cursor]) || field_body[cursor] == ',')) {
    ++cursor;
  }
  if (cursor == amount_start) {
    return Status::kBoundsError;
  }
  Status amt = ParseAmountMinor(field_body.substr(amount_start, cursor - amount_start),
                                out->debit_credit, &out->amount_minor);
  if (amt != Status::kOk) {
    return amt;
  }

  if (cursor + 4 <= field_body.size()) {
    out->transaction_type.assign(field_body.substr(cursor, 4));
    cursor += 4;
  }

  const std::size_t ref_start = cursor;
  while (cursor < field_body.size() && field_body[cursor] != '\n' &&
         field_body[cursor] != '\r') {
    ++cursor;
  }
  if (cursor > ref_start) {
    std::string_view refs = field_body.substr(ref_start, cursor - ref_start);
    const std::size_t slash = util::FindSubstring(refs, "//", 0);
    if (slash != std::string_view::npos) {
      out->customer_reference.assign(refs.substr(0, slash));
      out->bank_reference.assign(refs.substr(slash + 2));
    } else {
      out->customer_reference.assign(refs.data(), refs.size());
    }
  }
  return Status::kOk;
}

Status SwiftMt940Scanner::SplitSubfields86(std::string_view body,
                                           std::vector<std::string>* subfields) {
  if (subfields == nullptr) {
    return Status::kBoundsError;
  }
  subfields->clear();
  std::size_t cursor = 0;
  while (cursor < body.size()) {
    if (cursor + 3 <= body.size() && body[cursor] == '/' &&
        IsSwiftAlpha(body[cursor + 1])) {
      const std::size_t next = util::FindSubstring(body, "/", cursor + 2);
      if (next == std::string_view::npos) {
        subfields->emplace_back(body.substr(cursor));
        break;
      }
      subfields->emplace_back(body.substr(cursor, next - cursor));
      cursor = next;
      continue;
    }
    const std::size_t nl = body.find('\n', cursor);
    if (nl == std::string_view::npos) {
      subfields->emplace_back(body.substr(cursor));
      break;
    }
    subfields->emplace_back(body.substr(cursor, nl - cursor));
    cursor = nl + 1;
  }
  return Status::kOk;
}

Status SwiftMt940Scanner::ScanField86(std::string_view field_body,
                                      Mt940InfoSegment* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  out->raw_code.clear();
  out->narrative.clear();
  out->subfields.clear();

  std::string_view body = TrimSwift(field_body);
  if (body.empty()) {
    return Status::kTruncated;
  }

  if (body.size() >= 3 && body[0] == '/' && IsSwiftAlpha(body[1])) {
    const std::size_t end_code = body.find('/', 2);
    if (end_code != std::string_view::npos) {
      out->raw_code.assign(body.substr(0, end_code));
      body = body.substr(end_code);
    }
  }

  Status split = SplitSubfields86(body, &out->subfields);
  if (split != Status::kOk) {
    return split;
  }
  if (!out->subfields.empty()) {
    out->narrative = out->subfields.back();
  } else {
    out->narrative.assign(body.data(), body.size());
  }
  return Status::kOk;
}

Status SwiftMt940Scanner::ScanTaggedField(std::string_view tag,
                                          std::string_view value,
                                          Mt940Statement* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  if (tag == ":20:") {
    out->transaction_reference.assign(value.data(), value.size());
    ++fields_seen_;
    return Status::kOk;
  }
  if (tag == ":25:") {
    out->account_id.assign(value.data(), value.size());
    ++fields_seen_;
    return Status::kOk;
  }
  if (tag == ":28C:" || tag == ":28:") {
    out->statement_number.assign(value.data(), value.size());
    ++fields_seen_;
    return Status::kOk;
  }
  if (tag == ":60F:" || tag == ":60M:") {
    ++fields_seen_;
    return ParseBalance(value, &out->opening);
  }
  if (tag == ":62F:" || tag == ":62M:") {
    ++fields_seen_;
    return ParseBalance(value, &out->closing);
  }
  if (tag == ":61:") {
    Mt940StatementLine line;
    Status st = ScanField61(value, &line);
    if (st == Status::kOk) {
      out->lines.push_back(line);
      ++fields_seen_;
    }
    return st;
  }
  if (tag == ":86:") {
    Mt940InfoSegment info;
    Status st = ScanField86(value, &info);
    if (st == Status::kOk) {
      out->info_segments.push_back(info);
      ++fields_seen_;
    }
    return st;
  }
  return Status::kOk;
}

Status SwiftMt940Scanner::ScanBlock4(std::string_view block4, Mt940Statement* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  std::size_t cursor = 0;
  while (cursor < block4.size()) {
    if (block4[cursor] == '-' || block4[cursor] == '\r' ||
        block4[cursor] == '\n') {
      ++cursor;
      continue;
    }
    if (block4[cursor] != ':') {
      ++cursor;
      continue;
    }
    const std::size_t tag_end = block4.find(':', cursor + 1);
    if (tag_end == std::string_view::npos) {
      break;
    }
    std::string_view tag = block4.substr(cursor, tag_end - cursor + 1);
    cursor = tag_end + 1;
    const std::size_t value_start = cursor;
    std::size_t value_end = block4.size();
    const std::size_t next_tag = block4.find("\n:", cursor);
    if (next_tag != std::string_view::npos) {
      value_end = next_tag;
    }
    if (!util::SectionBodyInBounds(value_start, value_end - value_start,
                                   block4.size())) {
      return Status::kBoundsError;
    }
    std::string_view value = TrimSwift(block4.substr(value_start, value_end - value_start));
    Status st = ScanTaggedField(tag, value, out);
    if (st != Status::kOk) {
      return st;
    }
    cursor = value_end;
  }
  return Status::kOk;
}

Mt940ScanResult SwiftMt940Scanner::Scan(std::string_view swift_text) {
  Mt940ScanResult result{};
  result.status = Status::kOk;
  result.consumed_bytes = 0;
  result.line_count = 0;
  fields_seen_ = 0;

  std::string_view text = swift_text;
  const std::size_t block4_start = util::FindSubstring(text, "{4:", 0);
  if (block4_start != std::string_view::npos) {
    const std::size_t content_start = block4_start + 3;
    const std::size_t block4_end = text.find("-}", content_start);
    if (block4_end == std::string_view::npos) {
      result.status = Status::kTruncated;
      return result;
    }
    if (!util::SectionBodyInBounds(content_start, block4_end - content_start,
                                   text.size())) {
      result.status = Status::kBoundsError;
      return result;
    }
    text = text.substr(content_start, block4_end - content_start);
    result.consumed_bytes = block4_end + 2;
  } else {
    result.consumed_bytes = text.size();
  }

  result.status = ScanBlock4(text, &result.statement);
  result.line_count = static_cast<std::uint32_t>(result.statement.lines.size());
  if (result.consumed_bytes == 0) {
    result.consumed_bytes = swift_text.size();
  }
  return result;
}

}  // namespace wire
}  // namespace tkr
