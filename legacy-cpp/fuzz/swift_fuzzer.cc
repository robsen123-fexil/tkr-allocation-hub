#include "tkr/wire/swift_mt940_scanner.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  tkr::wire::SwiftMt940Scanner scanner;
  const std::string raw(reinterpret_cast<const char*>(data), size);
  scanner.Scan(raw);
  return 0;
}
