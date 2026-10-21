LOB-COMMANDS v1
# Reuse of dead ids: id 7 is filled, reused (rests), cancelled, reused again.
CONFIG min_price=1 max_price=1000 max_order_quantity=1000 max_live_orders=100
NEW 1 7 SELL 50 5
NEW 2 8 BUY 50 5
NEW 3 7 BUY 40 2
CANCEL 4 7
NEW 5 7 SELL 60 1
