#include "lob/core/config.hpp"

#include <stdexcept>

#include "lob/core/checked.hpp"

namespace lob {

void validate_config(const EngineConfig& config) {
  if (config.min_price < 1) {
    throw std::invalid_argument("EngineConfig: min_price must be >= 1");
  }
  if (config.max_price < config.min_price) {
    throw std::invalid_argument("EngineConfig: max_price must be >= min_price");
  }
  if (config.max_order_quantity < 1) {
    throw std::invalid_argument("EngineConfig: max_order_quantity must be >= 1");
  }
  if (config.max_live_orders < 1) {
    throw std::invalid_argument("EngineConfig: max_live_orders must be >= 1");
  }
  if (!checked_mul(config.max_price, config.max_order_quantity)) {
    throw std::invalid_argument(
        "EngineConfig: max_price * max_order_quantity overflows int64 "
        "(trade notional must be representable)");
  }
  if (!checked_mul(config.max_live_orders, config.max_order_quantity)) {
    throw std::invalid_argument(
        "EngineConfig: max_live_orders * max_order_quantity overflows int64 "
        "(resting quantity sums must be representable)");
  }
}

}  // namespace lob
