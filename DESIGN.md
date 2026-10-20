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

## 2. 订单簿结构 ✅（M2）

实现：`include/lob/book/order_book.hpp`、`src/book/order_book.cpp`。订单簿**不含撮合逻辑**，
只负责按价格-时间优先存放挂单并维持结构一致；"吃哪一单、吃多少"由 M3 决定。

```
OrderBook
 ├─ config_ : EngineConfig                                   // 构造时 validate_config
 ├─ bids_   : std::map<Price, PriceLevel, std::greater<>>    // begin() = 最优买价
 ├─ asks_   : std::map<Price, PriceLevel, std::less<>>       // begin() = 最优卖价
 ├─ index_  : std::unordered_map<OrderId, Locator>
 └─ last_seq_ : SeqNum                                       // 最近一次 add 的 seq
PriceLevel   { Quantity total_quantity; std::list<RestingOrder> orders; }   // FIFO
RestingOrder { OrderId id; Quantity remaining; SeqNum seq; }               // 侧与价由所在层隐含
Locator      { Side side; Price price; PriceLevel* level; std::list<RestingOrder>::iterator order; }
```

`Locator` 保存 `PriceLevel*` + 价格，而不是 map 迭代器：两侧 map 的比较器类型不同，迭代器类型也不同；
map 节点地址稳定，指针对两侧通用。代价是层被清空时需按价格 `erase(key)`，为 O(log L)（D-014）。

### 2.1 接口与契约

| 函数 | 作用 | 前置条件（违反 → `std::logic_error`，簿不变） | 异常保证 |
|---|---|---|---|
| `add(order, seq)` | 在价格层队尾挂单 | 通过 M1 无状态校验；ID 不在簿中；`order_count() < max_live_orders`；`seq > last_seq_`；不交叉也不锁定（买价 < 最优卖价，卖价 > 最优买价） | 强保证（含 `bad_alloc`） |
| `fill_front(side, qty)` | 对 `side` 侧最优价、最早的订单成交 `qty`；减到 0 即出队，层空即删层 | `side` 合法、非空；`0 < qty ≤ 队首剩余` | 前置检查先于修改；成功路径不分配内存、不抛出 |
| `cancel(id)` | 移除挂单，返回其撤单前状态 | 无：ID 不在簿中返回 `nullopt`（正常输入情形） | `noexcept`，不分配内存 |
| 查询 | `find / front / best_price / quantity_at / depth / level_count / order_count / for_each_order` | 带 `Side` 参数者要求 `is_valid(side)` | 除 `depth` 外均 `noexcept` |
| `find_invariant_violation()` | O(n) 全量一致性检查，返回首个违反项的描述 | — | 与 `NDEBUG` 无关 |

前置条件检查是 O(1) 的，Release 构建中同样执行（D-012）。违反意味着调用方（引擎）有 bug，而不是外部输入错误；
外部输入错误应由 M3 在调用前转化为拒单。

### 2.2 复杂度（L = 一侧价格层数，n = 簿中订单数）

| 操作 | 复杂度 | 堆分配次数（实测，见 §2.4） |
|---|---|---|
| `best_price` / `front` | O(1) | 0 |
| `add` 到已有价格层 | O(log L) 找层 + 均摊 O(1) 入队与入索引 | 2（链表节点 + 索引节点），触发 rehash 时 +1 |
| `add` 新价格层 | O(log L) | 3（+ map 节点），空簿首单或 rehash 时 +1 |
| `fill_front` 部分成交 | O(1) | 0 |
| `fill_front` 完全成交 | 均摊 O(1)（`erase(begin())`） | 0 |
| `cancel` 层仍非空 | 均摊 O(1)（哈希查找 + 缓存迭代器） | 0 |
| `cancel` 清空层 | O(log L)（按价格 `erase`） | 0 |
| `quantity_at` | O(log L) | 0 |
| `depth(k)` / `for_each_order` | O(k) / O(n) | `depth`：结果 vector 的增长分配；`for_each_order`：0 |
| `find_invariant_violation` | O(n) 均摊 | 字符串构造 |

### 2.3 迭代器 / 指针有效性论证

`Locator` 能缓存 `PriceLevel*` 和链表迭代器，依赖三条标准保证：

1. `std::map` 插入不使任何迭代器/引用失效；删除只使被删元素的迭代器/引用失效（[associative.reqmts]）。
2. `std::list` 同理（[list.modifiers]）。
3. `std::unordered_map` 的 rehash 使其**自身**迭代器失效，但不使元素的引用/指针失效（[unord.req]）；
   我们从不保存 `index_` 的迭代器，只保存 map/list 中的位置，所以 rehash 无影响。

所有删除路径（`fill_front` 完全成交、`cancel`）都在同一函数内同时删除订单节点和索引项，
`index_` 因而永远不含悬空的 `Locator`。不变量 4（索引双射，见下）在测试中每步验证这一点。

### 2.4 异常安全

- `add`：先完成全部前置检查（无修改），再按 `try_emplace 层 → push_back 节点 → emplace 索引` 顺序修改；
  每步只在后续步骤抛出时撤销前面已完成的步骤（删新建的空层、`pop_back` 节点），最后才更新 `total_quantity` 与 `last_seq_`
  （这两步不抛出）。各单步本身具有标准规定的强保证（单元素插入失败无副作用）。
- `fill_front` / `cancel`：成功路径只做减法与节点删除，不分配、不抛出；`fill_front` 的契约检查先于任何修改。
- 验证：`lob_book_alloc_tests` 替换全局 `operator new`，对每个 `add` 场景逐一让第 k 次分配失败，
  直到操作不再失败为止，并断言"注入点数 = 正常执行时的分配次数"——即每个分配点都被注入过一次；
  每次失败后断言簿与操作前逐字节一致、不变量成立、同一操作随后可成功。对 0..64 个订单规模的扫描共 158 个注入点，
  单次 `add` 最多 4 次分配（含 rehash）。`fill_front` / `cancel` 在"首次分配即失败"的条件下运行成功且分配计数为 0。

### 2.5 生命周期

- 禁止拷贝：成员逐一拷贝会让新对象的 `Locator` 指向源对象的节点。
- 移动构造/移动赋值 `noexcept`：容器移动转移节点所有权而不搬迁节点，`Locator` 继续指向同一批节点，
  归属变为目标对象。随后显式清空源对象的三个容器，使源对象成为与原配置、原 `last_seq_` 相同的**空且一致**的簿，
  而不依赖标准库对"被移动容器"的未指定状态（D-013）。自移动赋值为空操作。

### 2.6 结构不变量（`find_invariant_violation()`，测试中每步调用）

1. 不存在空价格层；层价格在配置价格带内。
2. 每层 `total_quantity` = 队列中 `remaining` 之和（求和用 `checked_add`，损坏状态下也不会溢出 UB）；每个 `remaining ∈ (0, max_order_quantity]`。
3. 每层队列 `seq` 严格递增（FIFO），且不超过 `last_seq_`。
4. 索引是到挂单的双射：每个挂单都有一个索引项，且该项的侧、价、层指针、节点地址都指向它；索引项总数 = 挂单总数。
5. `order_count() ≤ max_live_orders`。
6. 不交叉：两侧非空时 `best_bid < best_ask`。

每条不变量都有对应的 Tamper Test（故意破坏一项，断言报告的正是该项），证明检查器本身能检出问题。

### 2.7 备选方案（Level 2 实验候选，未引入）

侵入式链表 + 对象池（消除 `std::list` 与哈希节点的逐单分配，即 §2.2 中每次 `add` 的 2–4 次分配）；
价格带较窄时用按价格下标的扁平数组找层（O(1) 代替 O(log L)）；关闭 Release 下的契约检查以测其开销。
引入前须有剖析证据与基线对比。

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
