#pragma once

// The hand-built sample ITCH session used by the md tests (L3' 1d/1e) and,
// in larger generated form, by the pipeline tests. Its bytes are pinned by
// tests/golden/itch/session.hex.

#include <vector>

#include "lob/md/feed.hpp"
#include "lob/testkit/itch_builder.hpp"

namespace lob::testkit::itch_samples {

inline constexpr std::uint16_t kLocate = 7;  // ACME, the instrument under test
inline constexpr std::uint16_t kOther = 9;   // ZZZZ, filtered out

inline md::FeedConfig config() {
  md::FeedConfig c;
  c.symbol = md::make_symbol("ACME");
  c.book = md::BookConfig{64, 32};
  return c;
}

/// Directories, the trading phase (ACME trading, market open), orders for
/// ACME and for another stock, every subset message type once or more, one
/// non-subset message (Trade), market close.
/// Indices: 0 O · 1-2 directories · 3 H 'T' · 4 S 'Q' · 5.. order flow ·
/// 16 S 'M' · 17 S 'C'. (Index i has seq i + 1.)
inline std::vector<itch::Buf> session() {
  using namespace lob::testkit::itch;
  return {
      system_event('O', 1),
      stock_directory(kOther, "ZZZZ", 2),
      stock_directory(kLocate, "ACME", 3),
      trading_action(kLocate, "ACME", 'T', 4),
      system_event('Q', 5),
      add_order(kLocate, 1, 'B', 100, "ACME", 10000, 10),
      add_order(kOther, 2, 'B', 100, "ZZZZ", 5000, 11),
      add_order(kLocate, 3, 'S', 200, "ACME", 10100, 12),
      add_order_mpid(kLocate, 4, 'B', 50, "ACME", 10000, "MMID", 13),
      trade_message(kLocate, 14),
      order_executed(kLocate, 1, 40, 1, 15),
      order_executed_price(kLocate, 3, 20, 2, 'Y', 10090, 16),
      order_cancel(kLocate, 4, 10, 17),
      order_replace(kLocate, 3, 5, 150, 10050, 18),
      order_delete(kOther, 2, 19),
      order_delete(kLocate, 1, 20),
      system_event('M', 21),
      system_event('C', 22),
  };
}

inline itch::Buf session_stream() {
  itch::Buf out;
  for (const auto& m : session()) {
    itch::append_frame(out, m);
  }
  return out;
}

}  // namespace lob::testkit::itch_samples
