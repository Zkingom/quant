// Out-of-line so that callers see only VirtualStrategy and the comparison
// with the CRTP path really pays for the indirect call.
#include "lob/strategy/strategy.hpp"

namespace lob::strategy {

namespace {

class VirtualMarketMaker final : public VirtualStrategy {
 public:
  explicit VirtualMarketMaker(const MarketMakerParams& p) : mm_(p) {}
  void book_update(const BookTop& t, Actions& out) override { mm_.on_book_update(t, out); }
  void fill(const Fill& f, Actions& out) override { mm_.on_fill(f, out); }
  void timer(std::uint64_t ts, Actions& out) override { mm_.on_timer(ts, out); }

 private:
  MarketMaker mm_;
};

}  // namespace

std::unique_ptr<VirtualStrategy> make_virtual_market_maker(const MarketMakerParams& p) {
  return std::make_unique<VirtualMarketMaker>(p);
}

}  // namespace lob::strategy
