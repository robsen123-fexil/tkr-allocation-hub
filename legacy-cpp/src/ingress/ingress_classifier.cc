#include "tkr/ingress/ingress_classifier.h"

#include "tkr/util/bounds.h"

#include <algorithm>
#include <cctype>

namespace tkr {
namespace ingress {
namespace {

bool IsPrintableAscii(char ch) {
  return ch >= 0x20 && ch <= 0x7E;
}

bool HasFixPrefix(std::string_view bytes) {
  if (bytes.size() < 8) {
    return false;
  }
  return bytes.substr(0, 8) == "8=FIX.4.4";
}

bool HasSwiftBlockMarker(std::string_view bytes) {
  return util::FindSubstring(bytes, "{1:", 0) != std::string_view::npos ||
         util::FindSubstring(bytes, "{4:", 0) != std::string_view::npos ||
         util::FindSubstring(bytes, ":20:", 0) != std::string_view::npos ||
         util::FindSubstring(bytes, ":61:", 0) != std::string_view::npos;
}

float ClampConfidence(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

}  // namespace

IngressClassifier::IngressClassifier(IngressClassifierConfig config)
    : config_(config) {}

bool IngressClassifier::LooksLikeFix44(std::string_view bytes) {
  return HasFixPrefix(bytes);
}

bool IngressClassifier::LooksLikeSwiftMt940(std::string_view bytes) {
  return HasSwiftBlockMarker(bytes);
}

bool IngressClassifier::LooksLikeBatchMagic(std::uint32_t magic) {
  return magic == kBatchMagic;
}

bool IngressClassifier::LooksLikeEnvelopeMagic(std::uint32_t magic) {
  return magic == kEnvelopeMagic;
}

bool IngressClassifier::LooksLikeSessionMagic(std::uint32_t magic) {
  return magic == kSessionMagic;
}

std::uint32_t IngressClassifier::ReadMagicLe(const std::uint8_t* data,
                                             std::size_t size) const {
  if (data == nullptr || size < 4) {
    return 0;
  }
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::size_t IngressClassifier::CountPrintablePrefix(std::string_view bytes) const {
  std::size_t count = 0;
  for (char ch : bytes) {
    if (!IsPrintableAscii(ch) && ch != '\r' && ch != '\n' && ch != '\001') {
      break;
    }
    ++count;
  }
  return count;
}

IngressProbe IngressClassifier::ProbeFix44(std::string_view bytes) const {
  IngressProbe probe{};
  probe.kind = kIngressFix44;
  probe.magic_or_tag = 0;
  probe.frame_hint_bytes = 0;
  probe.confidence = 0.0f;

  if (bytes.size() < config_.min_fix_header_len) {
    return probe;
  }
  if (!HasFixPrefix(bytes)) {
    return probe;
  }
  probe.confidence = 0.55f;
  const std::size_t body_len_tag = util::FindSubstring(bytes, "\0019=", 0);
  if (body_len_tag != std::string_view::npos) {
    probe.confidence += 0.20f;
    probe.frame_hint_bytes = bytes.size();
  }
  const std::size_t msg_type = util::FindSubstring(bytes, "\00135=", 0);
  if (msg_type != std::string_view::npos) {
    probe.confidence += 0.15f;
  }
  const std::size_t checksum = util::FindSubstring(bytes, "\00110=", 0);
  if (checksum != std::string_view::npos) {
    probe.confidence += 0.10f;
  }
  probe.confidence = ClampConfidence(probe.confidence);
  return probe;
}

IngressProbe IngressClassifier::ProbeSwift(std::string_view bytes) const {
  IngressProbe probe{};
  probe.kind = kIngressSwiftMt940;
  probe.confidence = 0.0f;
  probe.frame_hint_bytes = bytes.size();

  if (HasSwiftBlockMarker(bytes)) {
    probe.confidence = 0.50f;
  }
  if (util::FindSubstring(bytes, ":61:", 0) != std::string_view::npos) {
    probe.confidence += 0.25f;
    probe.magic_or_tag = 61;
  }
  if (util::FindSubstring(bytes, ":86:", 0) != std::string_view::npos) {
    probe.confidence += 0.15f;
  }
  if (util::FindSubstring(bytes, ":60F:", 0) != std::string_view::npos ||
      util::FindSubstring(bytes, ":62F:", 0) != std::string_view::npos) {
    probe.confidence += 0.10f;
  }
  probe.confidence = ClampConfidence(probe.confidence);
  return probe;
}

IngressProbe IngressClassifier::ProbeBatchWire(const std::uint8_t* data,
                                               std::size_t size) const {
  IngressProbe probe{};
  probe.kind = kIngressBatchWire;
  probe.confidence = 0.0f;
  if (data == nullptr || size < 28) {
    return probe;
  }
  const std::uint32_t magic = ReadMagicLe(data, size);
  probe.magic_or_tag = magic;
  if (!LooksLikeBatchMagic(magic)) {
    return probe;
  }
  probe.confidence = 0.70f;
  const std::uint16_t version =
      static_cast<std::uint16_t>(data[4]) |
      (static_cast<std::uint16_t>(data[5]) << 8);
  if (version == kWireVersion) {
    probe.confidence += 0.15f;
  }
  const std::uint32_t record_count = ReadMagicLe(data + 8, size - 8);
  if (record_count <= kMaxBatchRecords) {
    probe.confidence += 0.10f;
    const std::size_t min_frame = 28 + static_cast<std::size_t>(record_count) * 32;
    probe.frame_hint_bytes = min_frame;
  }
  probe.confidence = ClampConfidence(probe.confidence);
  return probe;
}

IngressProbe IngressClassifier::ProbeEnvelopeWire(const std::uint8_t* data,
                                                  std::size_t size) const {
  IngressProbe probe{};
  probe.kind = kIngressEnvelopeWire;
  if (data == nullptr || size < 24) {
    return probe;
  }
  const std::uint32_t magic = ReadMagicLe(data, size);
  probe.magic_or_tag = magic;
  if (!LooksLikeEnvelopeMagic(magic)) {
    return probe;
  }
  probe.confidence = 0.75f;
  const std::uint16_t channel_count =
      static_cast<std::uint16_t>(data[6]) |
      (static_cast<std::uint16_t>(data[7]) << 8);
  if (channel_count <= kMaxEnvelopeChannels) {
    probe.confidence += 0.15f;
    probe.frame_hint_bytes = 24 + static_cast<std::size_t>(channel_count) * 20;
  }
  probe.confidence = ClampConfidence(probe.confidence);
  return probe;
}

IngressProbe IngressClassifier::ProbeSessionWire(const std::uint8_t* data,
                                                 std::size_t size) const {
  IngressProbe probe{};
  probe.kind = kIngressSessionWire;
  if (data == nullptr || size < 24) {
    return probe;
  }
  const std::uint32_t magic = ReadMagicLe(data, size);
  probe.magic_or_tag = magic;
  if (!LooksLikeSessionMagic(magic)) {
    return probe;
  }
  probe.confidence = 0.72f;
  const std::uint16_t leg_count =
      static_cast<std::uint16_t>(data[6]) |
      (static_cast<std::uint16_t>(data[7]) << 8);
  if (leg_count <= kMaxSessionLegs) {
    probe.confidence += 0.18f;
    probe.frame_hint_bytes = 24 + static_cast<std::size_t>(leg_count) * 28;
  }
  probe.confidence = ClampConfidence(probe.confidence);
  return probe;
}

IngressClassifyResult IngressClassifier::Classify(std::string_view bytes) {
  IngressClassifyResult result{};
  result.status = Status::kOk;
  result.primary_kind = kIngressUnknown;
  result.is_mixed_stream = false;
  result.leading_skip = 0;

  if (bytes.empty()) {
    result.status = Status::kTruncated;
    return result;
  }

  const std::size_t printable = CountPrintablePrefix(bytes);
  if (printable > 0 && printable < bytes.size()) {
    result.leading_skip = printable;
  }

  IngressProbe fix_probe = ProbeFix44(bytes);
  if (fix_probe.confidence > 0.0f) {
    result.probes.push_back(fix_probe);
  }

  IngressProbe swift_probe = ProbeSwift(bytes);
  if (swift_probe.confidence > 0.0f) {
    result.probes.push_back(swift_probe);
  }

  const std::uint8_t* bin_data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  IngressProbe batch_probe = ProbeBatchWire(bin_data, bytes.size());
  if (batch_probe.confidence > 0.0f) {
    result.probes.push_back(batch_probe);
  }
  IngressProbe envelope_probe = ProbeEnvelopeWire(bin_data, bytes.size());
  if (envelope_probe.confidence > 0.0f) {
    result.probes.push_back(envelope_probe);
  }
  IngressProbe session_probe = ProbeSessionWire(bin_data, bytes.size());
  if (session_probe.confidence > 0.0f) {
    result.probes.push_back(session_probe);
  }

  if (result.probes.empty()) {
    result.status = Status::kUnknownFormat;
    return result;
  }

  std::sort(result.probes.begin(), result.probes.end(),
            [](const IngressProbe& a, const IngressProbe& b) {
              return a.confidence > b.confidence;
            });

  result.primary_kind = result.probes.front().kind;

  if (result.probes.size() >= 2 && config_.accept_mixed) {
    const float delta = result.probes[0].confidence - result.probes[1].confidence;
    if (delta < 0.12f) {
      result.is_mixed_stream = true;
      result.primary_kind = kIngressMixedStream;
    }
  }

  return result;
}

IngressClassifyResult IngressClassifier::ClassifyBinary(const std::uint8_t* data,
                                                      std::size_t size) {
  if (data == nullptr || size == 0) {
    IngressClassifyResult result{};
    result.status = Status::kTruncated;
    return result;
  }
  std::string_view view(reinterpret_cast<const char*>(data), size);
  return Classify(view);
}

namespace {

bool ProbeHasAllocationTags(std::string_view bytes) {
  return util::FindSubstring(bytes, "\00178=", 0) != std::string_view::npos ||
         util::FindSubstring(bytes, "\00179=", 0) != std::string_view::npos ||
         util::FindSubstring(bytes, "\001467=", 0) != std::string_view::npos;
}

bool ProbeHasMarginSettlementTags(std::string_view bytes) {
  return util::FindSubstring(bytes, ":62F:", 0) != std::string_view::npos ||
         util::FindSubstring(bytes, ":60F:", 0) != std::string_view::npos;
}

float ScoreComplianceRelevance(const IngressProbe& probe, std::string_view bytes) {
  float bonus = 0.0f;
  if (probe.kind == kIngressFix44 && ProbeHasAllocationTags(bytes)) {
    bonus += 0.08f;
  }
  if (probe.kind == kIngressSwiftMt940 && ProbeHasMarginSettlementTags(bytes)) {
    bonus += 0.06f;
  }
  if (probe.kind == kIngressBatchWire &&
      probe.magic_or_tag == kBatchMagic) {
    bonus += 0.05f;
  }
  return bonus;
}

}  // namespace

IngressClassifyResult IngressClassifier::ClassifyWithComplianceHint(
    std::string_view bytes, bool require_margin_path) {
  IngressClassifyResult result = Classify(bytes);
  if (result.status != Status::kOk || result.probes.empty()) {
    return result;
  }

  for (IngressProbe& probe : result.probes) {
    probe.confidence = ClampConfidence(probe.confidence +
                                       ScoreComplianceRelevance(probe, bytes));
  }

  std::sort(result.probes.begin(), result.probes.end(),
            [](const IngressProbe& a, const IngressProbe& b) {
              return a.confidence > b.confidence;
            });
  result.primary_kind = result.probes.front().kind;

  if (require_margin_path) {
    bool margin_ok = false;
    for (const IngressProbe& probe : result.probes) {
      if (probe.kind == kIngressSwiftMt940 ||
          probe.kind == kIngressBatchWire ||
          probe.kind == kIngressEnvelopeWire) {
        margin_ok = true;
        break;
      }
    }
    if (!margin_ok) {
      result.status = Status::kMarginBreach;
    }
  }
  return result;
}

}  // namespace ingress
}  // namespace tkr
