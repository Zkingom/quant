LOB-COMMANDS v1
# Resting on both sides; cancel; cancel twice; cancel an unknown id.
# Expected: 1 and 2 rest; cancel 3 removes order 1; cancels 4 and 5 are UNKNOWN_ORDER_ID.
CONFIG min_price=1 max_price=1000 max_order_quantity=1000 max_live_orders=100
NEW 1 1 BUY 100 10
NEW 2 2 SELL 105 5
CANCEL 3 1
CANCEL 4 1
CANCEL 5 99
