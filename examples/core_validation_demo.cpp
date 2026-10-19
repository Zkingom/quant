// Demonstrates M1: config validation and stateless order validation.
// Prints one line per sample order: the order fields and the verdict.

#include <array>
#include <cstdio>
#include <exception>
#include <string_view>

#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/core/validation.hpp"

int main() {
  const lob::EngineConfig config{
      .min_price = 100, .max_price = 200, .max_order_quantity = 1'000, .max_live_orders = 1'000};

  try {
    lob::validate_config(config);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "invalid config: %s\n", e.what());
    return 1;
  }
  std::printf("config: price band [%lld, %lld], max qty %lld\n",
              static_cast<long long>(config.min_price), static_cast<long long>(config.max_price),
              static_cast<long long>(config.max_order_quantity));

  using lob::NewOrder;
  using lob::Side;
  const std::array samples{
      NewOrder{.id = 1, .side = Side::Buy, .price = 150, .quantity = 10},
      NewOrder{.id = 2, .side = Side::Sell, .price = 200, .quantity = 1'000},
      NewOrder{.id = 0, .side = Side::Buy, .price = 150, .quantity = 10},
      NewOrder{.id = 3, .side = static_cast<Side>(7), .price = 150, .quantity = 10},
      NewOrder{.id = 4, .side = Side::Buy, .price = 99, .quantity = 10},
      NewOrder{.id = 5, .side = Side::Sell, .price = 150, .quantity = 0},
      NewOrder{.id = 6, .side = Side::Sell, .price = 150, .quantity = 1'001},
  };

  for (const NewOrder& order : samples) {
    const auto reject = lob::validate_new_order(order, config);
    const std::string_view verdict = reject ? lob::to_string(*reject) : std::string_view{"OK"};
    const std::string_view side = lob::to_string(order.side);
    std::printf("id=%llu side=%.*s price=%lld qty=%lld -> %.*s\n",
                static_cast<unsigned long long>(order.id), static_cast<int>(side.size()),
                side.data(), static_cast<long long>(order.price),
                static_cast<long long>(order.quantity), static_cast<int>(verdict.size()),
                verdict.data());
  }
  return 0;
}
