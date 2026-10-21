LOB-COMMANDS v1
# Every reject reason expressible in text (INVALID_SIDE is not: unknown side tokens are
# syntax errors), in the documented check order; capacity 3.
# Expected: 1 INVALID_ORDER_ID, 2-3 PRICE_OUT_OF_RANGE, 4-5 INVALID_QUANTITY, 6 QUANTITY_TOO_LARGE,
# 7 rests, 8 DUPLICATE_ORDER_ID, 9-10 rest (book full), 11 BOOK_FULL, 12 trades with 1 and is
# accepted although the book is full, 13 rests in the freed slot, 14 INVALID_ORDER_ID (cancel),
# 15 UNKNOWN_ORDER_ID (order 1 was filled).
CONFIG min_price=10 max_price=20 max_order_quantity=100 max_live_orders=3
NEW 1 0 BUY 15 1
NEW 2 1 BUY 9 1
NEW 3 1 BUY 21 1
NEW 4 1 BUY 15 0
NEW 5 1 BUY 15 -3
NEW 6 1 BUY 15 101
NEW 7 1 BUY 15 1
NEW 8 1 SELL 16 1
NEW 9 2 BUY 14 1
NEW 10 3 BUY 13 1
NEW 11 4 BUY 12 1
NEW 12 5 SELL 15 1
NEW 13 6 SELL 19 2
CANCEL 14 0
CANCEL 15 1
