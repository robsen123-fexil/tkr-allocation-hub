#include "tkr/wire/fix44_session_parser.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  tkr::wire::Fix44SessionParser parser;
  const std::string raw(reinterpret_cast<const char*>(data), size);
  parser.ParseWithSession(raw);
  return 0;
}
