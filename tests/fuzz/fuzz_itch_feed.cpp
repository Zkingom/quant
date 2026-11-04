// libFuzzer entry for the market-data path (L3' 1e). Built only with
// -DLOB_FUZZ=ON and Clang (needs libclang-rt's fuzzer runtime); not part of
// CTest. Run, e.g.:
//   cmake -S . -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DLOB_FUZZ=ON
//   cmake --build build/fuzz --target lob_fuzz_itch_feed
//   build/fuzz/tests/lob_fuzz_itch_feed -max_total_time=60 tests/fuzz/corpus
// Every input is treated as a length-prefixed ITCH stream; the property is
// "no crash / no sanitizer report / book always consistent / no BookUpdate
// while not Live".

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "lob/md/feed.hpp"
#include "lob/md/stream.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  lob::md::FeedConfig cfg;
  cfg.symbol = lob::md::make_symbol("ACME");
  cfg.book = lob::md::BookConfig{64, 32};
  lob::md::FeedHandler h{cfg};
  std::vector<lob::md::FeedEvent> ev;
  ev.reserve(16);
  const auto sink = [&](const lob::md::FeedInput& in) {
    ev.clear();
    h.on_input(in, ev);
    for (const auto& e : ev) {
      if (e.kind == lob::md::FeedEvent::Kind::BookUpdate && e.state != lob::md::FeedState::Live) {
        std::abort();
      }
    }
  };
  const auto p = lob::md::decode_frames(lob::md::itch::Bytes{data, size}, 1, sink);
  if (p.last != lob::md::itch::FrameStatus::End) {
    lob::md::FeedInput fe{};
    fe.seq = p.next_seq;
    fe.kind = lob::md::InputKind::FrameError;
    sink(fe);
  }
  if (h.book().find_invariant_violation().has_value()) {
    std::abort();
  }
  return 0;
}
