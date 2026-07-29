#include "tkr/wire/fix44_session_parser.h"

#include "tkr/util/bounds.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>

namespace tkr {
namespace wire {
namespace {

constexpr std::uint32_t kTagBeginString = 8;
constexpr std::uint32_t kTagBodyLength = 9;
constexpr std::uint32_t kTagMsgType = 35;
constexpr std::uint32_t kTagSenderCompId = 49;
constexpr std::uint32_t kTagTargetCompId = 56;
constexpr std::uint32_t kTagMsgSeqNum = 34;
constexpr std::uint32_t kTagSendingTime = 52;
constexpr std::uint32_t kTagCheckSum = 10;
constexpr std::uint32_t kTagEncryptMethod = 98;
constexpr std::uint32_t kTagHeartBtInt = 108;
constexpr std::uint32_t kTagResetSeqNumFlag = 141;
constexpr std::uint32_t kTagClOrdId = 11;
constexpr std::uint32_t kTagOrderId = 37;
constexpr std::uint32_t kTagExecId = 17;
constexpr std::uint32_t kTagExecType = 150;
constexpr std::uint32_t kTagOrdStatus = 39;
constexpr std::uint32_t kTagSide = 54;
constexpr std::uint32_t kTagSymbol = 55;
constexpr std::uint32_t kTagLastQty = 32;
constexpr std::uint32_t kTagLastPx = 31;
constexpr std::uint32_t kTagCumQty = 14;
constexpr std::uint32_t kTagLeavesQty = 151;
constexpr std::uint32_t kTagAllocId = 70;
constexpr std::uint32_t kTagNoAllocs = 78;
constexpr std::uint32_t kTagAllocAccount = 79;
constexpr std::uint32_t kTagIndividualAllocId = 467;
constexpr std::uint32_t kTagAllocQty = 80;
constexpr std::uint32_t kTagAvgPx = 6;
constexpr std::uint32_t kTagGapFillFlag = 123;
constexpr std::uint32_t kTagNewSeqNo = 36;

bool IsDigitChar(char ch) {
  return ch >= '0' && ch <= '9';
}

std::uint32_t HashCompId(std::string_view text) {
  return util::Fnv1a32(text);
}

std::int64_t ParseFixDecimalToTicks(std::string_view value) {
  std::int64_t whole = 0;
  std::int64_t frac = 0;
  std::int64_t frac_digits = 0;
  bool seen_dot = false;
  bool negative = false;
  std::size_t idx = 0;
  if (!value.empty() && value[0] == '-') {
    negative = true;
    idx = 1;
  }
  for (; idx < value.size(); ++idx) {
    const char ch = value[idx];
    if (ch == '.') {
      seen_dot = true;
      continue;
    }
    if (!IsDigitChar(ch)) {
      break;
    }
    const std::int64_t digit = static_cast<std::int64_t>(ch - '0');
    if (!seen_dot) {
      whole = whole * 10 + digit;
    } else if (frac_digits < 6) {
      frac = frac * 10 + digit;
      ++frac_digits;
    }
  }
  std::int64_t ticks = whole * 10000;
  std::int64_t mul = 10000;
  for (std::int64_t i = 0; i < frac_digits; ++i) {
    mul /= 10;
  }
  ticks += frac * mul;
  return negative ? -ticks : ticks;
}

}  // namespace

Fix44SessionParser::Fix44SessionParser() {
  ResetSession();
}

void Fix44SessionParser::ResetSession() {
  session_ = FixSessionState{};
  session_.phase = FixSessionPhase::kDisconnected;
  session_.reset_on_logon = false;
  scratch_.clear();
}

void Fix44SessionParser::SetResetOnLogon(bool reset) {
  session_.reset_on_logon = reset;
}

FixMsgType Fix44SessionParser::MsgTypeFromTag(std::string_view value) {
  if (value == "D") {
    return FixMsgType::kNewOrderSingle;
  }
  if (value == "8") {
    return FixMsgType::kExecutionReport;
  }
  if (value == "J") {
    return FixMsgType::kAllocationInstruction;
  }
  if (value == "AS") {
    return FixMsgType::kAllocationReport;
  }
  if (value == "F") {
    return FixMsgType::kOrderCancelRequest;
  }
  if (value == "9") {
    return FixMsgType::kOrderCancelReject;
  }
  if (value == "H") {
    return FixMsgType::kOrderStatusRequest;
  }
  if (value == "AE") {
    return FixMsgType::kTradeCaptureReport;
  }
  if (value == "W") {
    return FixMsgType::kMarketDataSnapshot;
  }
  if (value == "j") {
    return FixMsgType::kBusinessMessageReject;
  }
  if (value == "A") {
    return FixMsgType::kUnknown;
  }
  if (value == "4") {
    return FixMsgType::kUnknown;
  }
  return FixMsgType::kUnknown;
}

bool Fix44SessionParser::ValidateChecksum(std::string_view raw) {
  if (raw.size() < 7) {
    return false;
  }
  const std::size_t soh = raw.rfind('\001');
  if (soh == std::string_view::npos || soh + 7 > raw.size()) {
    return false;
  }
  std::string_view tail = raw.substr(soh + 1);
  if (tail.size() < 3 || tail[0] != '1' || tail[1] != '0' || tail[2] != '=') {
    return false;
  }
  std::string_view checksum_text = tail.substr(3);
  const std::size_t end_soh = checksum_text.find('\001');
  if (end_soh != std::string_view::npos) {
    checksum_text = checksum_text.substr(0, end_soh);
  }
  std::int64_t expected = 0;
  if (!util::ParseAsciiInt(checksum_text, &expected)) {
    return false;
  }
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i < soh; ++i) {
    sum += static_cast<unsigned char>(raw[i]);
  }
  return static_cast<std::int64_t>(sum % 256) == expected;
}

Status Fix44SessionParser::SplitFixFrame(std::string_view raw,
                                         std::string_view* body,
                                         std::string_view* checksum_field) const {
  if (raw.size() < 10) {
    return Status::kTruncated;
  }
  if (raw.substr(0, 8) != "8=FIX.4.4") {
    return Status::kUnknownFormat;
  }
  const std::size_t len_tag = util::FindSubstring(raw, "\0019=", 0);
  if (len_tag == std::string_view::npos) {
    return Status::kTruncated;
  }
  const std::size_t len_value_start = len_tag + 3;
  const std::size_t len_value_end = raw.find('\001', len_value_start);
  if (len_value_end == std::string_view::npos) {
    return Status::kTruncated;
  }
  std::string_view len_text = raw.substr(len_value_start, len_value_end - len_value_start);
  std::int64_t body_length = 0;
  if (!util::ParseAsciiInt(len_text, &body_length) || body_length < 0) {
    return Status::kBoundsError;
  }
  const std::size_t body_start = len_value_end + 1;
  const std::size_t body_len = static_cast<std::size_t>(body_length);
  if (!util::SectionBodyInBounds(body_start, body_len, raw.size())) {
    return Status::kBoundsError;
  }
  *body = raw.substr(body_start, body_len);
  const std::size_t checksum_start = body_start + body_len;
  if (checksum_start >= raw.size()) {
    return Status::kTruncated;
  }
  *checksum_field = raw.substr(checksum_start);
  return Status::kOk;
}

Status Fix44SessionParser::TokenizeBody(std::string_view body, FixMessage* out) {
  if (out == nullptr) {
    return Status::kBoundsError;
  }
  out->fields.clear();
  out->tag_index.clear();
  out->msg_type = FixMsgType::kUnknown;

  std::size_t cursor = 0;
  while (cursor < body.size()) {
    const std::size_t eq = body.find('=', cursor);
    if (eq == std::string_view::npos) {
      break;
    }
    std::string_view tag_text = body.substr(cursor, eq - cursor);
    std::int64_t tag_num = 0;
    if (!util::ParseAsciiInt(tag_text, &tag_num) || tag_num < 0 ||
        tag_num > INT32_MAX) {
      return Status::kBoundsError;
    }
    const std::size_t value_start = eq + 1;
    const std::size_t value_end = body.find('\001', value_start);
    if (value_end == std::string_view::npos) {
      return Status::kTruncated;
    }
    const std::uint32_t tag = static_cast<std::uint32_t>(tag_num);
    std::string_view value = body.substr(value_start, value_end - value_start);

    FixTagValue field;
    field.tag = tag;
    field.value.assign(value.data(), value.size());
    out->fields.push_back(field);
    out->tag_index.emplace(tag, std::string_view(out->fields.back().value));

    if (tag == kTagMsgType) {
      out->msg_type = MsgTypeFromTag(value);
    }
    cursor = value_end + 1;
  }
  return Status::kOk;
}

Status Fix44SessionParser::ParseFieldBlock(std::string_view body, FixMessage* out) {
  return TokenizeBody(body, out);
}

FixParseResult Fix44SessionParser::ParseWithSession(std::string_view raw) {
  FixParseResult result{};
  result.status = Status::kOk;
  result.consumed_bytes = 0;
  result.checksum_valid = false;

  std::string_view body;
  std::string_view checksum_field;
  result.status = SplitFixFrame(raw, &body, &checksum_field);
  if (result.status != Status::kOk) {
    return result;
  }

  result.checksum_valid = ValidateChecksum(raw);
  result.status = TokenizeBody(body, &result.message);
  if (result.status != Status::kOk) {
    return result;
  }

  Status route_st = RouteMessage(result.message);
  if (route_st != Status::kOk) {
    result.status = route_st;
    return result;
  }

  const Status session_status = AdvanceSession(result.message);
  if (session_status != Status::kOk) {
    result.status = session_status;
    return result;
  }

  result.consumed_bytes = raw.size();
  return result;
}

bool Fix44SessionParser::LookupTag(const FixMessage& msg, std::uint32_t tag,
                                   std::string_view* out) const {
  if (out == nullptr) {
    return false;
  }
  const auto it = msg.tag_index.find(tag);
  if (it == msg.tag_index.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

bool Fix44SessionParser::LookupTagInt(const FixMessage& msg, std::uint32_t tag,
                                      std::int64_t* out) const {
  std::string_view text;
  if (!LookupTag(msg, tag, &text)) {
    return false;
  }
  return util::ParseAsciiInt(text, out);
}

Status Fix44SessionParser::ApplyLogon(const FixMessage& msg) {
  std::string_view sender;
  std::string_view target;
  if (!LookupTag(msg, kTagSenderCompId, &sender) ||
      !LookupTag(msg, kTagTargetCompId, &target)) {
    return Status::kTruncated;
  }
  session_.sender_comp_id_hash = HashCompId(sender);
  session_.target_comp_id_hash = HashCompId(target);
  std::int64_t reset_flag = 0;
  if (LookupTagInt(msg, kTagResetSeqNumFlag, &reset_flag) && reset_flag != 0) {
    session_.inbound_seq = 0;
    session_.outbound_seq = 0;
  }
  session_.phase = FixSessionPhase::kActive;
  return Status::kOk;
}

Status Fix44SessionParser::ApplySequenceReset(const FixMessage& msg) {
  std::int64_t new_seq = 0;
  if (!LookupTagInt(msg, kTagNewSeqNo, &new_seq) || new_seq < 0) {
    return Status::kBoundsError;
  }
  session_.inbound_seq = static_cast<std::uint32_t>(new_seq);
  session_.phase = FixSessionPhase::kActive;
  return Status::kOk;
}

Status Fix44SessionParser::ApplyExecutionReport(const FixMessage& msg) {
  std::string_view cl_ord;
  std::string_view exec_id;
  if (LookupTag(msg, kTagClOrdId, &cl_ord)) {
    session_.last_cl_ord_id.assign(cl_ord.data(), cl_ord.size());
  }
  if (LookupTag(msg, kTagExecId, &exec_id)) {
    session_.last_exec_id.assign(exec_id.data(), exec_id.size());
  }
  std::int64_t exec_type = 0;
  if (LookupTagInt(msg, kTagExecType, &exec_type)) {
    if (exec_type == 'F' || exec_type == '2') {
      session_.phase = FixSessionPhase::kAllocationPending;
    }
  }
  return Status::kOk;
}

Status Fix44SessionParser::ApplyAllocationInstruction(const FixMessage& msg) {
  session_.pending_legs.clear();
  std::vector<FixAllocationLeg> legs;
  const Status leg_status = ExtractAllocationLegs(msg, &legs);
  if (leg_status != Status::kOk) {
    return leg_status;
  }
  session_.pending_legs = std::move(legs);
  session_.phase = FixSessionPhase::kAllocationPending;
  return Status::kOk;
}

Status Fix44SessionParser::AdvanceSession(const FixMessage& msg) {
  std::int64_t seq = 0;
  if (LookupTagInt(msg, kTagMsgSeqNum, &seq)) {
    if (seq > 0) {
      const std::uint32_t expected = session_.inbound_seq + 1;
      if (session_.inbound_seq != 0 &&
          static_cast<std::uint32_t>(seq) > expected) {
        session_.phase = FixSessionPhase::kResendGap;
      }
      session_.inbound_seq = static_cast<std::uint32_t>(seq);
    }
  }

  std::string_view msg_type;
  if (!LookupTag(msg, kTagMsgType, &msg_type)) {
    return Status::kTruncated;
  }

  if (msg_type == "A") {
    return ApplyLogon(msg);
  }
  if (msg_type == "4") {
    return ApplySequenceReset(msg);
  }
  if (msg_type == "8") {
    return ApplyExecutionReport(msg);
  }
  if (msg_type == "J") {
    return ApplyAllocationInstruction(msg);
  }
  if (msg_type == "AS") {
    session_.phase = FixSessionPhase::kActive;
    session_.pending_legs.clear();
  }
  return Status::kOk;
}

Status Fix44SessionParser::ExtractAllocationLegs(
    const FixMessage& msg, std::vector<FixAllocationLeg>* legs) const {
  if (legs == nullptr) {
    return Status::kBoundsError;
  }
  legs->clear();

  std::int64_t group_count = 0;
  if (!LookupTagInt(msg, kTagNoAllocs, &group_count) || group_count <= 0) {
    return Status::kOk;
  }

  FixAllocationLeg current;
  bool in_group = false;
  for (const FixTagValue& field : msg.fields) {
    if (field.tag == kTagNoAllocs) {
      continue;
    }
    if (field.tag == kTagAllocAccount) {
      if (in_group) {
        legs->push_back(current);
        current = FixAllocationLeg{};
      }
      current.alloc_account = field.value;
      in_group = true;
      continue;
    }
    if (!in_group) {
      continue;
    }
    if (field.tag == kTagAllocQty) {
      std::int64_t qty = 0;
      if (util::ParseAsciiInt(field.value, &qty)) {
        current.last_qty = qty;
      }
    } else if (field.tag == kTagAvgPx) {
      current.avg_px_ticks = ParseFixDecimalToTicks(field.value);
    } else if (field.tag == kTagIndividualAllocId) {
      current.individual_alloc_id = field.value;
    }
  }
  if (in_group) {
    legs->push_back(current);
  }
  return Status::kOk;
}

Status Fix44SessionParser::ValidateOrderFields(const FixMessage& msg) const {
  std::string_view cl_ord_id;
  if (!LookupTag(msg, kTagClOrdId, &cl_ord_id) || cl_ord_id.empty()) {
    return Status::kTruncated;
  }
  std::string_view symbol;
  if (!LookupTag(msg, kTagSymbol, &symbol) || symbol.empty()) {
    return Status::kTruncated;
  }
  std::int64_t side = 0;
  if (!LookupTagInt(msg, kTagSide, &side)) {
    return Status::kBoundsError;
  }
  if (side != 1 && side != 2) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

Status Fix44SessionParser::ValidateExecutionFields(const FixMessage& msg) const {
  std::string_view exec_id;
  if (!LookupTag(msg, kTagExecId, &exec_id) || exec_id.empty()) {
    return Status::kTruncated;
  }
  std::int64_t exec_type = 0;
  if (!LookupTagInt(msg, kTagExecType, &exec_type)) {
    return Status::kBoundsError;
  }
  std::int64_t ord_status = 0;
  if (!LookupTagInt(msg, kTagOrdStatus, &ord_status)) {
    return Status::kBoundsError;
  }
  if (exec_type < 0 || exec_type > 8) {
    return Status::kBoundsError;
  }
  if (ord_status < 0 || ord_status > 8) {
    return Status::kBoundsError;
  }
  return Status::kOk;
}

bool Fix44SessionParser::IsAllocationMessage(const FixMessage& msg) const {
  std::string_view msg_type;
  if (!LookupTag(msg, kTagMsgType, &msg_type)) {
    return false;
  }
  return msg_type == "J" || msg_type == "AS" || msg_type == "AK";
}

Status Fix44SessionParser::ProcessHeartbeat(const FixMessage& msg) {
  std::int64_t test_req_id_present = 0;
  if (LookupTagInt(msg, 112, &test_req_id_present)) {
    session_.phase = FixSessionPhase::kActive;
  }
  return Status::kOk;
}

Status Fix44SessionParser::ProcessLogout(const FixMessage& msg) {
  (void)msg;
  session_.phase = FixSessionPhase::kDisconnected;
  session_.pending_legs.clear();
  return Status::kOk;
}

Status Fix44SessionParser::ProcessReject(const FixMessage& msg) {
  std::int64_t ref_seq = 0;
  LookupTagInt(msg, 45, &ref_seq);
  if (ref_seq > 0 && static_cast<std::uint32_t>(ref_seq) < session_.inbound_seq) {
    session_.phase = FixSessionPhase::kResendGap;
  }
  return Status::kOk;
}

Status Fix44SessionParser::ProcessResendRequest(const FixMessage& msg) {
  std::int64_t begin_seq = 0;
  std::int64_t end_seq = 0;
  LookupTagInt(msg, 7, &begin_seq);
  LookupTagInt(msg, 16, &end_seq);
  if (begin_seq > 0) {
    session_.phase = FixSessionPhase::kResendGap;
  }
  if (end_seq == 0) {
    session_.outbound_seq = session_.inbound_seq;
  }
  return Status::kOk;
}

Status Fix44SessionParser::ProcessAllocationReport(const FixMessage& msg) {
  std::vector<FixAllocationLeg> legs;
  Status st = ExtractAllocationLegs(msg, &legs);
  if (st != Status::kOk) {
    return st;
  }
  for (const FixAllocationLeg& leg : legs) {
    session_.pending_legs.push_back(leg);
  }
  session_.phase = FixSessionPhase::kActive;
  return Status::kOk;
}

Status Fix44SessionParser::ProcessOrderCancelRequest(const FixMessage& msg) {
  Status valid = ValidateOrderFields(msg);
  if (valid != Status::kOk) {
    return valid;
  }
  std::string_view cl_ord_id;
  LookupTag(msg, kTagClOrdId, &cl_ord_id);
  session_.last_cl_ord_id.assign(cl_ord_id.data(), cl_ord_id.size());
  return Status::kOk;
}

Status Fix44SessionParser::ProcessOrderCancelReject(const FixMessage& msg) {
  std::int64_t cxl_rej_reason = 0;
  LookupTagInt(msg, 102, &cxl_rej_reason);
  if (cxl_rej_reason > 0) {
    session_.phase = FixSessionPhase::kActive;
  }
  return Status::kOk;
}

Status Fix44SessionParser::RouteMessage(const FixMessage& msg) {
  std::string_view msg_type;
  if (!LookupTag(msg, kTagMsgType, &msg_type)) {
    return Status::kTruncated;
  }
  if (msg_type == "0") {
    return ProcessHeartbeat(msg);
  }
  if (msg_type == "5") {
    return ProcessLogout(msg);
  }
  if (msg_type == "3") {
    return ProcessReject(msg);
  }
  if (msg_type == "2") {
    return ProcessResendRequest(msg);
  }
  if (msg_type == "F") {
    return ProcessOrderCancelRequest(msg);
  }
  if (msg_type == "9") {
    return ProcessOrderCancelReject(msg);
  }
  if (msg_type == "AS" || msg_type == "AK") {
    return ProcessAllocationReport(msg);
  }
  if (msg_type == "D") {
    return ProcessNewOrderSingle(msg);
  }
  if (msg_type == "H") {
    return ProcessOrderStatusRequest(msg);
  }
  if (msg_type == "AE") {
    return ProcessTradeCaptureReport(msg);
  }
  if (msg_type == "W") {
    return ProcessMarketDataSnapshot(msg);
  }
  if (msg_type == "j") {
    return ProcessBusinessMessageReject(msg);
  }
  return Status::kOk;
}

Status Fix44SessionParser::ProcessNewOrderSingle(const FixMessage& msg) {
  Status valid = ValidateOrderFields(msg);
  if (valid != Status::kOk) {
    return valid;
  }

  std::string_view cl_ord_id;
  LookupTag(msg, kTagClOrdId, &cl_ord_id);
  session_.last_cl_ord_id.assign(cl_ord_id.data(), cl_ord_id.size());

  std::int64_t order_qty = 0;
  LookupTagInt(msg, 38, &order_qty);

  std::int64_t price = 0;
  LookupTagInt(msg, 44, &price);

  std::int64_t side = 0;
  LookupTagInt(msg, kTagSide, &side);

  if (order_qty <= 0) {
    return Status::kBoundsError;
  }
  if (side != 1 && side != 2) {
    return Status::kBoundsError;
  }

  session_.phase = FixSessionPhase::kActive;
  return Status::kOk;
}

Status Fix44SessionParser::ProcessOrderStatusRequest(const FixMessage& msg) {
  std::string_view cl_ord_id;
  if (!LookupTag(msg, kTagClOrdId, &cl_ord_id) || cl_ord_id.empty()) {
    return Status::kTruncated;
  }

  std::string_view order_id;
  LookupTag(msg, kTagOrderId, &order_id);

  if (order_id.empty() && cl_ord_id != session_.last_cl_ord_id) {
    return Status::kBoundsError;
  }

  return Status::kOk;
}

Status Fix44SessionParser::ProcessTradeCaptureReport(const FixMessage& msg) {
  Status valid = ValidateExecutionFields(msg);
  if (valid != Status::kOk) {
    return valid;
  }

  std::string_view exec_id;
  LookupTag(msg, kTagExecId, &exec_id);
  session_.last_exec_id.assign(exec_id.data(), exec_id.size());

  std::int64_t last_qty = 0;
  LookupTagInt(msg, kTagLastQty, &last_qty);

  std::int64_t last_px = 0;
  LookupTagInt(msg, kTagLastPx, &last_px);

  FixAllocationLeg leg{};
  std::string_view alloc_acct;
  if (LookupTag(msg, kTagAllocAccount, &alloc_acct)) {
    leg.alloc_account.assign(alloc_acct.data(), alloc_acct.size());
  }
  leg.last_qty = last_qty;
  leg.avg_px_ticks = last_px;

  if (last_qty > 0) {
    session_.pending_legs.push_back(leg);
  }

  return Status::kOk;
}

Status Fix44SessionParser::ProcessMarketDataSnapshot(const FixMessage& msg) {
  std::string_view symbol;
  if (!LookupTag(msg, kTagSymbol, &symbol) || symbol.empty()) {
    return Status::kTruncated;
  }

  std::int64_t md_req_id = 0;
  LookupTagInt(msg, 262, &md_req_id);

  std::int64_t no_entries = 0;
  LookupTagInt(msg, 268, &no_entries);

  if (no_entries < 0 || no_entries > 1000) {
    return Status::kBoundsError;
  }

  return Status::kOk;
}

Status Fix44SessionParser::ProcessBusinessMessageReject(const FixMessage& msg) {
  std::int64_t ref_msg_type = 0;
  LookupTagInt(msg, 372, &ref_msg_type);

  std::int64_t reason = 0;
  LookupTagInt(msg, 380, &reason);

  if (reason > 0) {
    session_.phase = FixSessionPhase::kActive;
  }

  if (ref_msg_type == 68) {
    session_.phase = FixSessionPhase::kAllocationPending;
  }

  return Status::kOk;
}

}  // namespace wire
}  // namespace tkr
