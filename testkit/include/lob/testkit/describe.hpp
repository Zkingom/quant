#pragma once

// Human-readable text for events, commands and books, used in failure
// messages and repro output. Debug format only: the M5 text format is the
// stable interchange format; do not parse this.

#include <string>
#include <vector>

#include "lob/book/order_book.hpp"
#include "lob/core/command.hpp"
#include "lob/engine/events.hpp"

namespace lob::testkit {

std::string describe(const Event& event);
std::string describe(const std::vector<Event>& events);  // one event per line
std::string describe(const Command& command);

/// Canonical text of the book: both sides in priority order, each order as
/// price/id x remaining #seq.
std::string dump(const OrderBook& book);

}  // namespace lob::testkit
