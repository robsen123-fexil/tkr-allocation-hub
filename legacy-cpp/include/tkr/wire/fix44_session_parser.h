#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tkr {
namespace wire {

enum class FixMsgType : std::uint8_t {
  kUnknown = 0,
  kNewOrderSingle,
  kExecutionReport,
  kAllocationInstruction,
  kAllocationReport,
  kOrderCancelRequest,
  kOrderCancelReject,
  kOrderStatusRequest,
  kTradeCaptureReport,
  kMarketDataSnapshot,
  kBusinessMessageReject,
};

enum class FixSessionPhase : std::uint8_t {
  kDisconnected = 0,
  kLogonPending,
  kActive,
  kAllocationPending,
  kResendGap,
};

struct FixTagValue {
  std::uint32_t tag;
  std::string value;
};

struct FixMessage {
  FixMsgType msg_type;
  std::vector<FixTagValue> fields;
  std::unordered_map<std::uint32_t, std::string_view> tag_index;
};

struct FixAllocationLeg {
  std::string alloc_account;
  std::int64_t last_qty;
  std::int64_t avg_px_ticks;
  std::string individual_alloc_id;
};

struct FixSessionState {
  FixSessionPhase phase;
  std::uint32_t inbound_seq;
  std::uint32_t outbound_seq;
  std::uint32_t sender_comp_id_hash;
  std::uint32_t target_comp_id_hash;
  std::string last_cl_ord_id;
  std::string last_exec_id;
  std::vector<FixAllocationLeg> pending_legs;
  bool reset_on_logon;
};

struct FixParseResult {
  Status status;
  FixMessage message;
  std::size_t consumed_bytes;
  bool checksum_valid;
};

class Fix44SessionParser {
 public:
  Fix44SessionParser();

  void ResetSession();
  void SetResetOnLogon(bool reset);

  FixParseResult ParseWithSession(std::string_view raw);
  Status ParseFieldBlock(std::string_view body, FixMessage* out);

  Status ExtractAllocationLegs(const FixMessage& msg,
                               std::vector<FixAllocationLeg>* legs) const;

  FixSessionPhase SessionPhase() const { return session_.phase; }
  std::uint32_t InboundSeq() const { return session_.inbound_seq; }
  const FixSessionState& Session() const { return session_; }

  static FixMsgType MsgTypeFromTag(std::string_view value);
  static bool ValidateChecksum(std::string_view raw);

 private:
  Status AdvanceSession(const FixMessage& msg);
  Status ApplyLogon(const FixMessage& msg);
  Status ApplySequenceReset(const FixMessage& msg);
  Status ApplyExecutionReport(const FixMessage& msg);
  Status ApplyAllocationInstruction(const FixMessage& msg);

  Status ValidateOrderFields(const FixMessage& msg) const;
  Status ValidateExecutionFields(const FixMessage& msg) const;
  bool IsAllocationMessage(const FixMessage& msg) const;
  Status ProcessHeartbeat(const FixMessage& msg);
  Status ProcessLogout(const FixMessage& msg);
  Status ProcessReject(const FixMessage& msg);
  Status ProcessResendRequest(const FixMessage& msg);
  Status ProcessAllocationReport(const FixMessage& msg);
  Status ProcessOrderCancelRequest(const FixMessage& msg);
  Status ProcessOrderCancelReject(const FixMessage& msg);
  Status ProcessNewOrderSingle(const FixMessage& msg);
  Status ProcessOrderStatusRequest(const FixMessage& msg);
  Status ProcessTradeCaptureReport(const FixMessage& msg);
  Status ProcessMarketDataSnapshot(const FixMessage& msg);
  Status ProcessBusinessMessageReject(const FixMessage& msg);
  Status RouteMessage(const FixMessage& msg);

  bool LookupTag(const FixMessage& msg, std::uint32_t tag,
                 std::string_view* out) const;
  bool LookupTagInt(const FixMessage& msg, std::uint32_t tag,
                    std::int64_t* out) const;

  Status SplitFixFrame(std::string_view raw, std::string_view* body,
                       std::string_view* checksum_field) const;
  Status TokenizeBody(std::string_view body, FixMessage* out);

  FixSessionState session_;
  std::vector<char> scratch_;
};

}  // namespace wire
}  // namespace tkr
