#pragma once

#include "tkr/ingress/ingress_classifier.h"
#include "tkr/types.h"
#include "tkr/wire/batch_wire_codec.h"
#include "tkr/wire/fix44_session_parser.h"
#include "tkr/wire/swift_mt940_scanner.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace tkr {
namespace ingress {

struct IngressDispatchConfig {
  bool queue_envelope_channels;
  bool parse_fix_sessions;
  bool parse_swift_statements;
  std::uint32_t max_pending_envelopes;
};

struct IngressStreamStats {
  std::uint32_t frames_seen;
  std::uint32_t envelopes_queued;
  std::uint32_t batches_decoded;
  std::uint32_t fix_messages;
  std::uint32_t swift_statements;
  std::uint32_t rejected_frames;
};

struct IngressStreamResult {
  Status status;
  IngressStreamStats stats;
  std::vector<IngressEnvelope> completed_envelopes;
  std::vector<BatchWireFrame> decoded_batches;
};

class IngressDispatch {
 public:
  explicit IngressDispatch(IngressDispatchConfig config);

  IngressStreamResult ProcessIngressStream(const std::uint8_t* data,
                                           std::size_t size);
  IngressStreamResult ProcessIngressStream(std::string_view text);

  Status QueueEnvelopeChannelViews(IngressEnvelope* envelope);
  Status SealPendingEnvelopes();
  Status CommitIngressSweep();
  void Reset();

  const std::deque<IngressEnvelope>& PendingEnvelopes() const {
    return pending_envelopes_;
  }
  const IngressStreamStats& Stats() const { return stats_; }

 private:
  Status DispatchFixSegment(std::string_view segment, IngressStreamResult* out);
  Status DispatchSwiftSegment(std::string_view segment, IngressStreamResult* out);
  Status DispatchBatchSegment(const std::uint8_t* data, std::size_t size,
                              IngressStreamResult* out);
  Status DispatchEnvelopeSegment(const std::uint8_t* data, std::size_t size,
                                 IngressStreamResult* out);
  Status DispatchSessionSegment(const std::uint8_t* data, std::size_t size,
                                IngressStreamResult* out);

  Status BuildEnvelopeFromWire(const wire::BatchDecodeResult& batch_result,
                               IngressEnvelope* envelope);
  Status AttachChannelViews(IngressEnvelope* envelope);

  IngressDispatchConfig config_;
  IngressClassifier classifier_;
  wire::Fix44SessionParser fix_parser_;
  wire::SwiftMt940Scanner swift_scanner_;
  wire::BatchWireCodec batch_codec_;

  std::deque<IngressEnvelope> pending_envelopes_;
  std::vector<std::unique_ptr<std::vector<std::uint8_t>>> temp_payload_heap_;
  IngressStreamStats stats_;
  std::uint32_t ingress_seq_;
};

}  // namespace ingress
}  // namespace tkr
