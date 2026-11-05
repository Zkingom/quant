// libFuzzer entry for the OUCH 4.2 codec (L4.1). Built only with
// -DLOB_FUZZ=ON (Clang); run with scripts/fuzz_ouch.sh.
//
// Each input is decoded as an inbound and as an outbound payload. Pass
// criterion: no crash / sanitizer report, and every Decoded payload
// re-encodes to exactly the input bytes (the codec keeps every field raw).

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lob/ouch/ouch.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  namespace ouch = lob::ouch;
  const ouch::Bytes in{data, size};
  std::uint8_t buf[ouch::kMaxMessageSize];
  {
    ouch::Inbound m;
    if (ouch::decode_inbound(in, m) == ouch::DecodeStatus::Decoded) {
      const std::size_t n = ouch::encode(m, buf);
      if (n != size || std::memcmp(buf, data, size) != 0) std::abort();
    }
  }
  {
    ouch::Outbound m;
    if (ouch::decode_outbound(in, m) == ouch::DecodeStatus::Decoded) {
      const std::size_t n = ouch::encode(m, buf);
      if (n != size || std::memcmp(buf, data, size) != 0) std::abort();
    }
  }
  return 0;
}
