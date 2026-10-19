# DESIGN

> ✅ = 已实现并测试；📝 = 规格/计划，尚未实现，实施时可能修订（修订记录写入 DECISIONS.md）。

## 1. 基本类型 ✅（M1）

| 类型 | 表示 | 合法域 | 说明 |
|---|---|---|---|
| `Price` | `std::int64_t`，单位 tick | `[min_price, max_price]`，`min_price ≥ 1` | 显示用 tick size 不进入核心 |
| `Quantity` | `std::int64_t`，单位 lot | `(0, max_order_quantity]` | 有符号：负值可被检测而不是回绕 |
| `OrderId` | `std::uint64_t` | `≠ 0` | 0 保留为"无订单" |
| `SeqNum` | `std::uint64_t` | 单调递增 | 引擎内部时间优先的唯一来源 |
| `Side` | `enum class : uint8_t {Buy, Sell}` | 仅两个枚举值 | 来自二进制/强转的其他值被校验拒绝 |

### 1.1 配置约束 ✅

`EngineConfig { min_price, max_price, max_order_quantity, max_live_orders }`，由 `validate_config()`
在引擎构造时检查，违反即抛 `std::invalid_argument`（启动期错误，不在热路径上）：

1. `min_price ≥ 1`
2. `max_price ≥ min_price`
3. `max_order_quantity ≥ 1`
4. `max_live_orders ≥ 1`
5. `max_price × max_order_quantity` 不溢出 `int64` → 任意单笔成交名义金额可表示。
6. `max_live_orders × max_order_quantity` 不溢出 `int64` → 任意价格层/一侧的数量合计可表示。

约束 6 把"层总量会不会溢出"从一个依赖内存大小的经验论断变成了由配置保证的定理：
活跃订单数 ≤ `max_live_orders`（由 M3 以 `BOOK_FULL` 拒单强制），每单剩余 ≤ `max_order_quantity`，
所以任何合计 ≤ 二者之积 ≤ `INT64_MAX`。

### 1.2 无状态校验 ✅

`validate_new_order(order, config)` 按**固定顺序**检查，返回第一个违反项：

| 顺序 | 条件 | 拒单原因 |
|---|---|---|
| 1 | `id == 0` | `INVALID_ORDER_ID` |
| 2 | `side ∉ {Buy, Sell}` | `INVALID_SIDE` |
| 3 | `price ∉ [min_price, max_price]` | `PRICE_OUT_OF_RANGE` |
| 4 | `quantity ≤ 0` | `INVALID_QUANTITY` |
| 5 | `quantity > max_order_quantity` | `QUANTITY_TOO_LARGE` |

`validate_cancel_order(cancel)`：`id == 0` → `INVALID_ORDER_ID`。

检查顺序是可观察行为（会出现在事件输出与 Golden 文件中），因此被测试固定。

有状态的拒单原因（`DUPLICATE_ORDER_ID`、`UNKNOWN_ORDER_ID`、`BOOK_FULL`）已在 M1 定义，由 M3 判定。

## 2. 订单簿结构 📝（M2，基线方案）

```
OrderBook
 ├─ bids_ : std::map<Price, PriceLevel, std::greater<>>   // begin() = 最优买价
 ├─ asks_ : std::map<Price, PriceLevel, std::less<>>      // begin() = 最优卖价
 └─ index_: std::unordered_map<OrderId, Locator>
PriceLevel { std::list<RestingOrder> queue; Quantity total; }
RestingOrder { OrderId id; Quantity remaining; SeqNum seq; }
Locator { Side side; map::iterator level; list::iterator pos; }
```

| 操作 | 复杂度 |
|---|---|
| 最优价查询 | O(1) |
| 挂单（已有价格层） | O(log L) 找层 + O(1) 入队 |
| 挂单（新价格层） | O(log L) |
| 撤单 | O(1) 均摊（哈希查找 + 保存的迭代器直接删除） |
| 成交一个 maker | O(1)；清空价格层 O(1) 均摊（`erase(begin())`） |

L = 一侧价格层数。`std::map` 与 `std::list` 迭代器在其他元素增删时保持有效，这是 `Locator`
可以缓存迭代器的前提；只有删除该元素本身才会使之失效，而删除时会同步删除索引项。

结构不变量（`check_invariants()` 逐条检查，测试中每步调用）：

1. 不存在空价格层。
2. 每层 `total` = 队列中 `remaining` 之和；每个 `remaining ∈ (0, max_order_quantity]`。
3. 每层队列 `seq` 严格递增（FIFO）。
4. `index_.size()` = 两侧订单总数；每个索引项指回同 ID、同侧、同价的订单。
5. 不交叉：两侧非空时 `best_bid < best_ask`。

备选方案（列入 Level 2 实验候选，不在基线引入）：侵入式链表 + 对象池（去掉 `std::list` 逐节点分配）；
按价格索引的扁平数组（价格带窄时 O(1) 找层）。

## 3. 撮合语义 📝（M3）

- 订单类型：仅限价单，GTC（未成交部分挂单）。不支持市价单、IOC/FOK、改单、冰山单。
- 价格优先、同价时间优先（按引擎分配的 `SeqNum`）。
- 买单可与 `ask ≤ limit` 的卖单成交；卖单可与 `bid ≥ limit` 的买单成交。
- **成交价 = maker（挂单方）价格。**
- 逐 maker 成交，`fill = min(taker.remaining, maker.remaining)`；maker 剩余为 0 即出队，层空即删层。
- 无自成交保护（单标的、无账户概念）。
- 重复 ID：与**当前活跃**订单的 ID 冲突时拒绝；已成交/已撤订单的 ID 允许复用（见 DECISIONS D-008）。
- 撤单：ID 不在活跃索引中 → `UNKNOWN_ORDER_ID`。
- 容量：接受新订单会使活跃订单数可能超过 `max_live_orders` 时拒绝（`BOOK_FULL`）。具体判定点（撮合前/后）在 M3 确定并写入 DECISIONS。

事件（计划）：`Accepted`、`Rejected{reason}`、`Trade{taker, maker, price, qty}`、`Rested{remaining}`、`Cancelled{remaining}`，
均带全局递增序号。

## 4. 快照 📝（M5）

规范文本：卖方价格升序、买方价格降序，层内按 FIFO 列出 `id remaining`；附 FNV-1a 64 位哈希。
