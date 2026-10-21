LOB-COMMANDS v1
# Price priority, time priority within a price, maker-price execution.
# Expected: buy 5 (limit 103, qty 10) trades 2@101x3, 3@101x3, 1@102x3, 4@103x1; maker 4 keeps 2.
CONFIG min_price=1 max_price=1000 max_order_quantity=1000 max_live_orders=100
NEW 1 1 SELL 102 3
NEW 2 2 SELL 101 3
NEW 3 3 SELL 101 3
NEW 4 4 SELL 103 3
NEW 5 5 BUY 103 10
