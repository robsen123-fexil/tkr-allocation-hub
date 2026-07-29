#pragma once

#include "tkr/types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace tkr {
namespace ingress {

struct IngressProbe {
  IngressKind kind;
  float confidence;
  std::uint32_t magic_or_tag;
  std::size_t frame_hint_bytes;
};

struct IngressClassifyResult {
  Status status;
  IngressKind primary_kind;
  std::vector<IngressProbe> probes;
  bool is_mixed_stream;
  std::size_t leading_skip;
};

struct IngressClassifierConfig {
  bool accept_mixed;
  bool require_magic_alignment;
  std::uint32_t min_fix_header_len;
};

class IngressClassifier {
 public:
  explicit IngressClassifier(IngressClassifierConfig config);

  IngressClassifyResult Classify(std::string_view bytes);
  IngressClassifyResult ClassifyBinary(const std::uint8_t* data, std::size_t size);
  IngressClassifyResult ClassifyWithComplianceHint(std::string_view bytes,
                                                   bool require_margin_path);

  static bool LooksLikeFix44(std::string_view bytes);
  static bool LooksLikeSwiftMt940(std::string_view bytes);
  static bool LooksLikeBatchMagic(std::uint32_t magic);
  static bool LooksLikeEnvelopeMagic(std::uint32_t magic);
  static bool LooksLikeSessionMagic(std::uint32_t magic);

 private:
  IngressProbe ProbeFix44(std::string_view bytes) const;
  IngressProbe ProbeSwift(std::string_view bytes) const;
  IngressProbe ProbeBatchWire(const std::uint8_t* data, std::size_t size) const;
  IngressProbe ProbeEnvelopeWire(const std::uint8_t* data, std::size_t size) const;
  IngressProbe ProbeSessionWire(const std::uint8_t* data, std::size_t size) const;

  std::size_t CountPrintablePrefix(std::string_view bytes) const;
  std::uint32_t ReadMagicLe(const std::uint8_t* data, std::size_t size) const;

  IngressClassifierConfig config_;
};

}  // namespace ingress
}  // namespace tkr
