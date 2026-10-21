LOB-COMMANDS v1
# Multi-level sweep, taker remainder rests, maker partial fills.
# Expected: sell 4 (99x12) trades 1@100x5, 2@99x5 and rests 2 at 99 (98 does not cross);
# buy 5 (99x1) takes 1 of maker 4; buy 6 (101x4) takes maker 4's last lot at 99 and rests 3 at 101.
CONFIG min_price=1 max_price=1000 max_order_quantity=1000 max_live_orders=100
NEW 1 1 BUY 100 5
NEW 2 2 BUY 99 5
NEW 3 3 BUY 98 5
NEW 4 4 SELL 99 12
NEW 5 5 BUY 99 1
NEW 6 6 BUY 101 4
