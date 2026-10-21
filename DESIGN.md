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

## 3. 撮合引擎 ✅（M3）

实现：`include/lob/engine/{events,matching_engine}.hpp`、`src/engine/matching_engine.cpp`。

### 3.1 接口

```cpp
class MatchingEngine {
 public:
  explicit MatchingEngine(const EngineConfig& config);      // 非法配置抛 invalid_argument
  void process(const Command& command, std::vector<Event>& events);
  void process(const NewOrder& order, std::vector<Event>& events);
  void process(const CancelOrder& cancel, std::vector<Event>& events);
  const OrderBook& book() const noexcept;
  SeqNum last_seq() const noexcept;                          // 最近分配的事件序号；初始 0
  std::optional<std::string> find_invariant_violation() const;
};
```

- `process` 把本命令产生的事件**追加**到调用方的 `events`（不清空已有内容）。
- 不可拷贝（内含 `OrderBook`），可 `noexcept` 移动；被移动后的引擎簿为空、保留 `last_seq()`，可继续使用。
- 引擎只通过 M2 的公共接口操作订单簿；M2 为此只新增一个只读查询 `for_each_order_while`（D-018）。

### 3.2 事件模型

| 事件 | 字段 | 产生时机 |
|---|---|---|
| `OrderAccepted` | seq, id, side, price, quantity | 新订单通过全部校验 |
| `OrderRejected` | seq, command(New/Cancel), id, reason | 任一校验失败；**簿不变** |
| `TradeExecuted` | seq, taker_id, maker_id, taker_side, price, quantity, taker_remaining, maker_remaining | 每与一个 maker 成交一次 |
| `OrderRested` | seq, id, side, price, remaining | taker 剩余量挂入簿 |
| `OrderCancelled` | seq, id, side, price, remaining, reason(Requested / InsufficientResources) | 撤单成功；或剩余量因内存不足未能挂单（§3.5） |

- **单一时钟**：每个事件分配一个 `seq`，从 1 开始逐一递增，不跳号；被拒命令也消耗一个 `seq`。
- **时间优先 = 受理序号**：挂单在簿中的优先级 `seq` 等于该订单 `OrderAccepted.seq`（不是 `OrderRested.seq`）。
  由于 `seq` 全局单调，这满足 M2 `add` 的"seq 严格递增"前置条件。
- 一个新订单的事件序列恰为：`Accepted · Trade* · (Rested | ∅)`；`∅` 表示完全成交（最后一条 Trade 的 `taker_remaining == 0`）。
  被拒时只有一条 `Rejected`。撤单为 `Cancelled(Requested)` 或 `Rejected`。
- 全部事件类型可平凡拷贝；`Event = std::variant<...>` 的拷贝不分配、不抛出（`static_assert`）。

### 3.3 撮合语义

- 仅限价单、GTC。无市价单、IOC/FOK、改单、冰山单、自成交保护。
- 可成交条件：买单与 `ask ≤ limit` 的卖单、卖单与 `bid ≥ limit` 的买单。**等价也成交**。
- 价格优先，同价按受理 `seq` 先后；逐 maker 成交 `q = min(taker 剩余, maker 剩余)`；**成交价 = maker 价格**。
- 一次可扫过多个价格层，直到 taker 剩余为 0、对手侧为空或对手最优价不再可成交。
- 剩余量（若 > 0）以原限价、受理 `seq` 挂入簿。挂入时簿必然不交叉：可成交的流动性已被耗尽。

### 3.4 校验与拒单（全部在任何状态改变之前完成）

新订单按以下**固定顺序**判定，首个失败即拒：

| 顺序 | 检查 | 拒单原因 |
|---|---|---|
| 1–5 | M1 无状态校验（id、side、价格带、数量 > 0、数量 ≤ 上限） | `INVALID_ORDER_ID` … `QUANTITY_TOO_LARGE` |
| 6 | ID 与簿中**活跃**订单相同 | `DUPLICATE_ORDER_ID` |
| 7 | 预演撮合后存在剩余量，且 `簿中订单数 − 将被完全成交的 maker 数 ≥ max_live_orders` | `BOOK_FULL` |

撤单：`id == 0` → `INVALID_ORDER_ID`；ID 不在簿中 → `UNKNOWN_ORDER_ID`（含已成交、已撤销、从未出现）。

`BOOK_FULL` 的取舍（D-021）：只在订单**确实需要挂单**且挂单后会超出上限时才拒绝；
会完全成交、或通过成交腾出空间的订单不受影响。判定依赖预演结果，因此预演必须先于任何修改（§3.5）。
被拒订单不产生任何成交（全有或全无），不会出现"先成交一部分、再因容量拒绝剩余"的情况。

**推论：`BOOK_FULL` 只会拒绝完全不会成交的订单。** 证明：若剩余 > 0，则每个被触及的 maker 都被完全成交
（否则剩余会继续与它成交），即 `makers_filled = trades`；簿中订单数始终 ≤ `max_live_orders`，
所以若 `trades ≥ 1`，则 `count − makers_filled ≤ max − 1 < max`，条件不成立。差分测试对每一次 `BOOK_FULL` 断言
"拒单前对手侧不存在可成交的挂单"（D-021）。

`seq` 耗尽（`uint64` 回绕）在提交前检查，若本命令所需的事件数会使 `seq` 溢出则抛 `std::overflow_error`，状态不变。
实际不可达（每秒 10⁹ 个事件也需约 584 年），但检查是 O(1) 的，且保证 `seq` 的单调性永不被破坏。

### 3.5 两阶段处理与异常安全

```
process(NewOrder)
 ── 准备阶段（只读；可能抛出；抛出时引擎状态与 events 内容均不变）──
   1. 无状态校验 → 重复 ID → 预演撮合（for_each_order_while 只读遍历对手侧，O(k)）→ BOOK_FULL
   2. 计算本命令的事件数 n = 1 + 成交数 + (剩余 > 0)；检查 seq 不溢出
   3. 确保 events 容量 ≥ size + n（几何增长，避免逐命令精确 reserve 导致 O(N²)）
 ── 提交阶段 ──
   4. 追加 Accepted                                     不分配（容量已预留）
   5. 循环 front(对手侧) + fill_front，追加 Trade       不分配（M2 实测）、前置条件由预演保证
   6. 若剩余 > 0：book.add(剩余, 受理 seq)              唯一可能失败的步骤：仅 std::bad_alloc
        成功 → 追加 Rested
        bad_alloc → add 自身强保证（簿未改变）→ 追加 Cancelled(InsufficientResources) → 重抛
```

**保证级别（逐情形）**：

| 情形 | 保证 |
|---|---|
| 被拒的新订单 / 撤单（含被拒撤单） | 强：准备阶段失败时什么都没发生；成功路径只追加一个事件 |
| 新订单完全成交，或没有剩余 | 强：唯一可能的失败在准备阶段 |
| 新订单有剩余，且 `add` 因内存不足失败 | **基本保证 + 完整审计**（不是强保证）：已执行的成交保留；剩余量未挂单；事件流以 `Cancelled(InsufficientResources)` 如实结束该订单的生命周期；不变量与数量守恒成立；`bad_alloc` 传播给调用方 |

**为什么剩余挂单失败时无法提供强保证**：回滚需要把已被完全成交、已出队的 maker 以原 `seq` 放回其价格层的**队首**。
M2 的 `add` 只能入队尾并要求 `seq` 严格递增，因此不存在这样的恢复操作；而剩余量只能在成交之后挂入
（M2 拒绝交叉订单）。在不修改 M2 的前提下，强保证不可实现。可选的改进（预分配节点池使挂单不分配，从而整个提交阶段不抛出）
列为 L2 候选，需另行论证。

**提交阶段不分配的依据与验证**：`fill_front` 与 `front` 不分配（M2 分配注入测试，分配计数 0）；
`events.push_back` 在已预留容量内不分配，且 `Event` 拷贝不抛出。M3 分配注入测试对每个注入点分类：
要么 events 与引擎状态完全不变（准备阶段），要么属于上表第三行；断言不存在第三类结果。

### 3.6 引擎级不变量

由 `find_invariant_violation()` 检查（引擎内部）：

- I1 订单簿结构不变量（M2 全部六条）。
- I2 簿中每个挂单的 `seq ≤ last_seq()`。

由测试侧的**事件审计器**从事件流独立验证（不依赖引擎内部状态）：

- A1 `seq` 从 1 开始逐一递增。
- A2 生命周期状态机：`Accepted → Trade(taker)* → {Rested | 完全成交 | Cancelled(InsufficientResources)}`；
  `Rested → Trade(maker)* → {完全成交 | Cancelled(Requested)}`；终结后的订单不再出现在事件中（除非 ID 被重新受理）。
- A3 每笔成交：maker 是审计器重建簿中该侧**当时的最优价最早单**（价格-时间优先）；成交价 = maker 价格；
  价格对 taker 可成交；`0 < q ≤` 双方成交前剩余；`taker_remaining`/`maker_remaining` 与审计器计算一致。
- A4 `Rested` 时对手侧不存在可与之成交的挂单（无"穿价挂单"）。
- A5 数量守恒（每条命令后）：每个订单 `原始数量 = Σ成交 + 当前剩余/终结剩余`；
  全局 `Σ受理数量 = 2·Σ成交数量 + Σ挂单剩余 + Σ撤销剩余`。
- A6 审计器重建的簿（含每单 `seq`）与引擎的簿逐单相等。

说明：A5 在审计器**自身**的重建上逐条命令检查时，由于每个数量都已逐事件核对，它在构造上必然成立，不构成独立证据；
有独立意义的是针对**引擎**簿的形式：`Σ受理 = 2·Σ成交 + Σ引擎簿中剩余 + Σ撤销`，在 `matches()` 中检查。
`BOOK_FULL` 的合法性（是否真的需要拒绝）审计器不判定，由差分测试与参考引擎比较。

### 3.7 测试设计

| 层次 | 内容 | 判定依据 |
|---|---|---|
| 单元测试 | §3.3/§3.4 的每条规则一个具名测试，事件序列逐字段断言 | 手算期望 |
| 审计器自检 | 构造违反 A1–A5 的事件流，断言审计器报告对应条目 | 证明审计器能发现问题 |
| 参考引擎 + 差分 | 独立的朴素实现（扁平 vector、线性扫描、在副本上模拟撮合）；固定种子随机命令流；每条命令后比较完整事件序列与完整簿；同时运行审计器 | 两个独立实现逐事件一致 + 性质检查 |
| 分配失败注入 | 对 `process` 的每个分配点注入失败，按 §3.5 分类并与参考引擎比较 | 保证级别的实证 |
| 变异测试 | 人工注入撮合/校验/异常处理缺陷，确认被杀死 | 测试的检出能力 |

## 4. 测试基础设施 ✅（M4）

实现：`testkit/`（CMake 目标 `lob::testkit`、`lob_testkit_alloc_fault`、`lob_soak`）。只在 `LOB_BUILD_TESTS` 时构建，
不进入任何生产目标；不依赖 GoogleTest（M5 回放测试与 M6 基准可直接复用）。

### 4.1 组件与职责

| 组件 | 文件 | 职责 | 消费者 |
|---|---|---|---|
| `Rng` | `rng.hpp` | 跨平台确定的种子随机数（`mt19937_64` + 自有区间映射） | 全部生成器 |
| `ReferenceEngine` | `reference_engine.hpp` | 独立朴素实现（扁平 vector、线性扫描、副本上模拟撮合）；`best_price`（O(n)）；`drop_resting`（仅用于故障后对齐） | 差分、契约、生成器的状态视图 |
| `EventAuditor` | `event_auditor.hpp/.cpp` | A1–A6 性质检查；128 位累计量 | 单元、差分、长跑 |
| `WorkloadProfile` / `WorkloadGenerator` | `workload.hpp/.cpp` | 10 个工作负载画像；按当前模型状态生成命令 | 差分、长跑、契约；M6 基准（计划） |
| `run_differential` / `Coverage` / `unmet_targets` | `differential.hpp/.cpp` | 引擎 vs 参考 + 审计；全量检查间隔；状态指标；可选故障注入；边界目标判定 | gtest 测试、`lob_soak` |
| `alloc` 注入 | `alloc_fault.hpp/.cpp`（OBJECT 库） | 替换全局 `operator new/delete`；`FaultWindow`；`Injector` 适配器 | `*_alloc_tests`、`lob_soak` |
| `describe` / `dump` | `describe.hpp/.cpp` | 事件、命令、簿的调试文本 | 失败信息、复现输出 |
| `lob_soak` | `tools/soak.cpp` | 长跑 CLI：画像 × 种子区间、时间与资源、目标、复现命令 | `scripts/soak.sh` |

仍留在 `tests/` 的：M2 订单簿参考模型 `tests/book/reference_book.hpp`（只有 M2 一个消费者）、GoogleTest 打印胶水 `engine_test_util.hpp`。

### 4.2 差分执行器的检查节奏

| 每条命令 | 每 `full_check_every` 条命令、每次注入故障后、运行结束时 |
|---|---|
| 引擎与参考的完整事件向量相等；审计器 A1–A5（审计器自身重建）；`BOOK_FULL` 只拒不可成交订单；两侧最优档位采样 | 两侧完整簿逐单相等（含 seq）；审计器 `matches()`（A5 针对引擎簿、A6）；引擎不变量（M2 六条 + I2）；全深度档位数采样 |

全量检查不消耗随机数，故障注入使用独立的随机流（由种子派生），因此 **`--check-every` 不改变命令流**
（测试 `CheckIntervalDoesNotChangeTheStream` 对 10 个画像逐一验证）：长跑中用稀疏检查发现的失败，用 `--check-every 1` 复现即可定位到确切步。

### 4.3 长序列故障注入

每条命令以 `fault_per_mille / 1000` 的概率武装"第 k 次分配失败"（k ∈ [0, 5]），一半情况下先清空事件缓冲区的容量。结果分类：

- **PREPARE**：事件为空、`last_seq` 未变 → 参考引擎跳过该命令；立即全量检查（证明簿未变）。
- **RESIDUAL**：参考引擎的事件（末尾 `Rested` 换成 `Cancelled(INSUFFICIENT_RESOURCES)`）必须与引擎完全一致；`last_seq` 等于该撤销事件的 seq；
  参考引擎 `drop_resting` 对齐；审计器接受；立即全量检查。
- 其他任何结果（吞掉注入的失败、非注入的 `bad_alloc`、其他异常、事件变化但参考不挂单）→ 失败。

与 M3 的区别：M3 每个注入点后只验证一条后续命令；M4 在同一次运行中反复注入并**继续运行**数万条命令。

### 4.4 工作负载画像与边界目标

成功标准不是步数，而是**画像是否到达它声称的状态边界**；长跑以 `--require-targets` 运行，未达到即失败。

| 画像 | 目标边界 | 目标（长跑须满足） |
|---|---|---|
| mixed | M3 基线（回归锚点） | 逐字复现 M3 的计数；全部 8 种拒单；满载受理；容量上限 |
| deep_book | 深簿 | 单侧 ≥ 1000 档；≥ 90% 容量（4500 单）；单条命令扫过 ≥ 20 档 |
| single_price | 价格带只有一个价（min = max） | 队列 ≥ 200 单；全部成交在该价；单条命令成交 ≥ 20 个 maker |
| high_density | 多数订单到达即成交 | ≥ 70% 受理订单到达即成交；≥ 50% 成交使 maker 部分成交 |
| thin_book | 对手侧被扫空 | ≥ 5% 的命令扫空一侧 |
| cancel_heavy | 索引高频变动 | 成功撤单 ≥ 20% 命令；未知 ID 撤单 ≥ 10% |
| id_churn | ID 生命周期（ID 空间 64） | 重复 ID 拒单 ≥ 5% 新单；失效 ID 复用 ≥ 5% 受理 |
| capacity_edge | 容量边界（上限 4） | `BOOK_FULL` ≥ 10% 新单；满载受理 ≥ 1% 命令；达到上限 |
| extreme_quantity | D-006 上界 | 某档位总量 ≥ INT64_MAX/4；累计受理量 > INT64_MAX（128 位记账） |
| extreme_price | 价格接近表示上限 | 价格带边缘受理 ≥ 1%；边缘成交 > 0 |

校准记录（未达标时修改画像而非目标；唯一一次修改目标附有结构性论证，见 MODULE_NOTES/M4 §6）。

## 5. M5 接口契约（由 `tests/engine/test_replay_contract.cpp` 固定）

M5（持久化、回放、恢复）调用 `MatchingEngine::process` 时可以依赖、也必须防范的行为：

| 条款 | 内容 | 测试 |
|---|---|---|
| C1 | `process` 抛出且 `last_seq()` 未变 ⇒ 命令完全没有生效，可安全重试，结果与无故障运行相同 | `C1NotAppliedCommandCanBeRetried` |
| C2 | `process` 抛出且 `last_seq()` 已变 ⇒ 命令**已生效**（成交保留，事件以 `Cancelled(INSUFFICIENT_RESOURCES)` 结束）。**不得重试**：重试会再次受理并成交（已演示） | `C2RetryAfterAppliedFailureExecutesTwice` |
| C3 | 发生过资源失败的运行，**不能**通过重新执行命令日志复现：该命令的剩余量在重放中会挂单，之后的命令可能与之成交（已演示） | `C3CommandReplayDivergesAfterAResourceFailure` |
| C4 | **事件日志单独**足以重建精确的簿（含优先级 seq），包括跨越多次资源失败 | `C4C5EventLogRebuildsBookAndSeqRangesAttributeEvents` |
| C5 | 一次 `process` 产生的事件恰好是 seq 区间 `(last_seq 之前, last_seq 之后]`，连续，无论是否抛出 | 同上 |
| C6 | 无资源失败时，相同命令日志在新引擎中重放得到逐字节相同的事件日志，与调用方缓冲区策略无关 | `C6FailureFreeCommandReplayIsExact` |

由此得出的 M5 必须解决的问题，见 MODULE_NOTES/M4_testkit.md §8。

## 6. 确定性回放、快照与恢复 ✅（M5）

实现：引擎新增快照导出/恢复接口（`lob::engine`）；新模块 `lob::io`（文本格式、日志、快照文件、恢复、会话）；CLI `lob_replay`。
生产组件不依赖 `lob::testkit`；测试通过 `ByteSink` 接口注入写失败，通过 testkit 注入分配失败。

### 6.1 三种标识及其关系

| 标识 | 谁分配 | 粒度 | 作用 |
|---|---|---|---|
| `cmd_seq`（命令序号） | 输入方（命令文件 / 网关） | 每条命令一个，从 1 起**连续**递增 | 命令的身份；去重键；"已提交到哪条命令"的水位 |
| `seq`（事件序号） | 引擎 | 每个事件一个，连续递增 | 事件全序；时间优先 |
| 日志位置（字节偏移） | 日志写入器 | 每个批次的起止偏移 | 恢复时的截断点；只在内部使用 |

一条命令 ↔ 日志中的一个**批次**：`BATCH cmd_seq n 命令原文` + n 个事件 + `COMMIT cmd_seq 校验和`。
批次 k 的事件 seq 必须紧接批次 k−1 的最后一个 seq（C5），恢复时逐批校验。每条被处理的命令至少产生 1 个事件（拒单也是事件），
因此 n ≥ 1；"未生效"的命令（准备阶段失败）不写批次。

### 6.2 特别审查问题

**Q1 `last_seq()` 何时足以判定命令已生效？**
足以：**同一进程内、紧接着一次 `process()` 调用之后**——`last_seq` 前进 ⟺ 该调用追加了事件 ⟺ 命令已在内存中生效（M3 两阶段语义；M4 C1/C2/C5 已验证）。
不够：
- 跨崩溃/重启：内存中的 `last_seq` 已丢失；必须以**持久化日志中最后一个完整批次**为准。
- 持久化失败：命令已在内存中生效，但从未提交；它在语义上**不算发生**（§6.4），而 `last_seq` 已前进。
- `logic_error`：`last_seq` 可能已前进，但事件流不完整、不可信（R3），不能据此判定。
- 识别**哪条**命令：`last_seq` 是事件级的，一条命令的事件数不定；命令身份只能由 `cmd_seq` 给出。
- 快照恢复后：`last_seq` 的连续性依赖快照记录的 `last_seq`，而不是重新计数。
结论：`last_seq` 只用于单次调用内"已生效/未生效"的分类；跨越持久化边界的判定一律用"日志中 `cmd_seq` 的完整批次"。

**Q2 同一命令在异常、崩溃、重启、重试后的语义**

| 情形 | 内存 | 持久化 | 对外确认 | 后续对同一 `cmd_seq` 的处理 |
|---|---|---|---|---|
| 准备阶段 `bad_alloc` | 未生效 | 无 | 无 | 可重试（会话自动重试有限次） |
| 提交阶段 `bad_alloc`（RESIDUAL） | 已生效（成交保留，剩余撤销） | 写入批次（以 `CANCELLED … INSUFFICIENT_RESOURCES` 结束） | 提交后确认 | 视为已提交；再次提交 → `DUPLICATE`，**不再执行** |
| 写日志失败 | 已生效 | 未提交（可能有残缺尾部） | 无 | 会话中毒、停止；重启后恢复到最后提交点，该命令**重新执行**（它从未对外可见） |
| 写入后、确认前崩溃 | 丢失 | 批次完整 ⇒ 已提交；不完整 ⇒ 未提交 | 无 | 已提交 → 重启后 `DUPLICATE`；未提交 → 重新执行 |
| 确认后崩溃 | 丢失 | 已提交（持久模式下确认在 fsync 之后） | 已确认 | `DUPLICATE` |
| 客户端重试同一 `cmd_seq`、相同内容 | — | — | — | `≤ 已提交水位` → `DUPLICATE`（返回原批次的 seq 区间）；`= 水位+1` → 正常执行 |
| 重试同一 `cmd_seq`、**不同内容** | — | — | — | 拒绝（`CMD_SEQ_REUSED`），不执行 |
| `cmd_seq` 跳号 | — | — | — | 拒绝（`CMD_SEQ_GAP`），不执行 |

**Q3 处理成功但持久化失败时，如何阻止未提交结果对外可见？**
事件在批次写入（持久模式下还要 `fsync`）成功之后才交给确认输出；写入失败时会话进入**中毒**状态：拒绝一切后续命令、不再输出、
不暴露订单簿。内存中领先于日志的状态随进程一起丢弃；对外唯一可见的状态是"最后提交点"，重启时从快照 + 日志重建。

**Q4 一条命令产生多个事件时，如何避免只恢复一部分？**
批次是恢复的最小单位：只有带有正确 `COMMIT` 行（`cmd_seq` 一致、事件数一致、FNV-1a 64 校验和覆盖整个批次文本）的批次才会被应用；
批次以**一次** `write` 写出。崩溃留下的残缺批次没有有效的 `COMMIT`，整个丢弃。

**Q5 快照与事件日志之间的一致恢复边界**
快照只在**提交边界**（某批次已持久化之后、下一条命令之前）生成，记录 `last_cmd`（已提交水位）与 `last_seq`；
写入方式为临时文件 + `fsync` + `rename` + 目录 `fsync`，文件末尾有校验和。恢复时要求：日志中 `cmd_seq = last_cmd` 的批次存在且其最后一个 seq 等于快照的
`last_seq`（否则快照与日志不属于同一条历史，报错）；然后只应用 `cmd_seq > last_cmd` 的批次。也可以不用快照，从空状态折叠整个日志。

**Q6 日志尾部部分写入或损坏的检测与处理**
扫描日志，最后一个有效批次之后的字节按以下规则分类：
- **干净**：恰好结束于 `COMMIT` 行之后。
- **残缺尾部（torn）**：剩余字节（去掉末尾的 NUL 填充后）中，所有**完整**的行都构成一个合法批次的前缀，且没有完整的 `COMMIT` 行
  （最后一行可以不完整）——这是"写到一半崩溃"的形态。恢复丢弃这些字节，报告丢弃的字节数，并在继续写入之前把文件**截断**到有效长度
  （否则新批次会接在垃圾之后，下一次恢复就变成中间损坏）。
- **损坏（corrupt）**：出现格式不合法的完整行、`COMMIT` 存在但校验和/事件数/`cmd_seq` 不符、seq 不连续，或损坏之后还有数据 → 拒绝自动恢复，报告位置。
已知局限：如果损坏恰好只改动了最后一个 `COMMIT` 行末尾的换行符，它与"最后一个字节没写完"无法区分，会被当作残缺尾部丢弃；
测试对最后一个批次的每个字节做修改，断言要么报告损坏、要么丢弃整个批次，**从不接受被修改的批次**。

**Q7 R3 / R10 / R11**

| 异常 | 来源 | 会话策略 |
|---|---|---|
| `std::logic_error`（R3） | 程序错误（计划与执行不一致、M2 契约违反） | 中毒并停止；不写批次、不确认；退出码区分；重启会在同一命令处再次失败 → 需要修复代码（报告触发的 `cmd_seq`） |
| `std::length_error`（R10） | 准备阶段预留事件容量 | 未生效（强保证）；确定性失败，重试无意义 → 返回 `REFUSED`，不中毒，CLI 停止并报告 |
| `std::overflow_error`（R11，seq 将回绕） | 准备阶段 | 同上；可以用快照恢复构造 `last_seq` 接近上限的引擎来测试（不再需要白盒访问） |
| 其他未知异常 | — | 同 `logic_error` |

**Q8 M4 契约 C1–C6 如何扩展为恢复系统的验收测试**

| M4 契约 | M5 验收测试 |
|---|---|
| C1 未生效可重试 | 会话在准备阶段失败后自动重试；含注入的运行与无故障运行产生**逐字节相同的日志** |
| C2 已生效禁止重试 | 会话把 RESIDUAL 批次提交；重复提交同一 `cmd_seq` 得到 `DUPLICATE`，簿与日志不变（风险被会话消除） |
| C3 命令重放会偏离 | `lob_replay verify` 只在含 `INSUFFICIENT_RESOURCES` 的批次报告"结果取自日志"，其余批次逐事件一致；恢复使用事件折叠，状态精确 |
| C4 事件日志足以重建 | 生产恢复（快照 + 日志折叠 → `MatchingEngine::restore`）在多种画像、多种长度、任意切点上与原引擎状态相等，且**之后的事件**逐一相同 |
| C5 seq 区间归属 | 日志批次记录 `cmd_seq` 与事件数；扫描器校验跨批次 seq 连续 |
| C6 无故障重放精确 | Golden 测试（逐字节）；同一命令文件两次运行、GCC/Clang 运行的日志与快照逐字节相同 |

### 6.3 文本格式 v1

全部为 ASCII、`\n` 结尾、字段以单个空格分隔；数字为十进制、无前导零、无 `+`；**拒绝** `\r`（规范形式唯一，便于逐字节比较）。
首行是带版本号的魔数；版本不符即报错。

```
命令文件                          日志（journal）                            快照
LOB-COMMANDS v1                   LOB-JOURNAL v1                            LOB-SNAPSHOT v1
CONFIG <四项配置>                 CONFIG <四项配置>                          CONFIG <四项配置>
NEW <cmd> <id> <BUY|SELL> <p> <q> BATCH <cmd> <n> <命令原文>                LAST_CMD <cmd>
CANCEL <cmd> <id>                 <事件行> × n                               LAST_SEQ <seq>
# 注释、空行（仅命令文件）        COMMIT <cmd> <16 位十六进制 FNV-1a 64>     ORDERS <n>
                                                                            ORDER <side> <p> <id> <rem> <seq> × n
                                                                            END <16 位十六进制 FNV-1a 64>
```

`CONFIG min_price=… max_price=… max_order_quantity=… max_live_orders=…`（固定顺序）。事件行：

```
ACCEPTED <seq> <id> <side> <price> <qty>
REJECTED <seq> <NEW|CANCEL> <id> <REASON>
TRADE <seq> <taker> <maker> <taker_side> <price> <qty> <taker_rem> <maker_rem>
RESTED <seq> <id> <side> <price> <rem>
CANCELLED <seq> <id> <side> <price> <rem> <REQUESTED|INSUFFICIENT_RESOURCES>
```

非法 `Side` 值（M1 允许的二进制强转值）在文本中不可表达；文本解析器把未知侧向令牌报告为语法错误。价格与数量可以是任意 `int64`（含 0 与负数），
交由引擎拒单——这样文本格式可以表达每一种引擎拒单。

### 6.4 会话（`lob::io::Session`）状态机与提交协议

```
          open(快照?, 日志)                       submit(cmd_seq, cmd)
  ┌──────────┐  恢复成功、截断残缺尾部   ┌────────┐ ──cmd_seq ≤ 水位──────────▶ DUPLICATE（内容比对）
  │  Closed  │ ───────────────────────▶ │ Ready  │ ──cmd_seq ≠ 水位+1─────────▶ CMD_SEQ_GAP
  └──────────┘                           └────────┘ ──准备阶段 bad_alloc────────▶ 重试 ≤ R 次，仍失败 → NOT_APPLIED
                                          │   ▲     ──length/overflow_error───▶ REFUSED
                         写日志失败 /      │   │     ──提交成功────────────────▶ COMMITTED / COMMITTED_WITH_RESOURCE_FAILURE
                         logic_error /     ▼   │                                  （确认输出、可能写快照）
                         未知异常       ┌──────────┐
                                        │ Poisoned │ 拒绝一切；只能丢弃会话并重新 open（恢复到最后提交点）
                                        └──────────┘
```

提交步骤（每条命令）：
1. 处理：`engine.process`（内存生效或未生效，§Q2）。
2. 持久化：把整个批次编码为一个缓冲区，`sink.write`（一次调用）；持久模式下 `sink.sync`（`fsync`）。**这是提交点。**
3. 确认：把事件交给确认输出（CLI 中为标准输出 / 事件文件）。
4. 可选：每 N 条命令写一次快照（失败只报告，不影响已提交的日志）。

三种状态不是原子的：1 成功而 2 失败 → 中毒、不确认；2 成功而 3 之前崩溃 → 已提交但客户端未收到确认，重启后重试得到 `DUPLICATE`
（会附带原批次的 seq 区间，客户端可以据此从日志取回结果）。

### 6.5 恢复（`lob::io::recover`）

```
recover(快照?, 日志字节)
  1. 解析日志头（版本、配置）；扫描批次：校验框架、校验和、cmd_seq 连续、seq 连续；分类尾部（干净 / 残缺 / 损坏）
  2. 初始状态 = 快照（校验其校验和、配置一致、与日志的边界一致）或空状态
  3. 事件折叠：对 cmd_seq > 起点的每个批次，按事件更新状态（ACCEPTED→待定；TRADE→改 maker 剩余；RESTED→挂入；CANCELLED→移除），
     并校验事件与批次中的命令一致、与当前状态一致（不一致即损坏）
  4. 得到 EngineSnapshot + 已提交水位 + 每个 cmd_seq 的命令摘要（供去重时比对内容）+ 日志有效长度
  5. MatchingEngine::restore(状态)（再次全面校验）
```

恢复以**事件**为真源（D-032），因此含资源失败的历史也能精确恢复；命令只用于去重比对与 `verify` 模式的再执行校验。

### 6.6 保证边界（如实声明）

- **确定性**：无资源失败时，相同命令文件 ⇒ 逐字节相同的日志与快照（跨运行、跨 GCC/Clang；均为 libstdc++）。
- **进程崩溃一致性（已测试）**：模型为"进程在任意时刻被杀死，日志文件保留了已写出字节的任意前缀"。测试方法：对日志在每个字节偏移截断后恢复；
  对真实进程 `kill -9` 后重启并续跑，最终日志与不中断运行逐字节相同；持久模式下，已确认的批次在恢复后全部存在。
- **写失败（已测试）**：注入的写入/同步失败使会话中毒；恢复得到最后提交点。
- **掉电 / 操作系统崩溃持久性：未验证。** 依赖文件系统正确实现 `fsync` 与 `rename`；开发环境中仓库位于 WSL 的 9P 文件系统（`/mnt/g`），
  其 `fsync` 语义未知，崩溃测试在 ext4（`/tmp`）上进行。因此本机制在掉电场景下是**受限恢复机制**。
- **不声称 exactly-once。** 提供的是：每个 `cmd_seq` 在一条恢复谱系内**至多生效一次**（去重），且只在提交后确认；
  "恰好一次"还需要客户端在收到确认前以同一 `cmd_seq`、同一内容重试，这超出本系统的范围。
- 默认（非持久）模式只在结束时 `fsync`：崩溃可能丢失已确认的批次，仅用于回放与测试，不提供持久性。

### 6.7 实现中修正设计的发现

1. **提交点之后不得再分配内存。** 初版会话在批次持久化之后才计算命令摘要并追加提交记录（两者都分配内存）。
   若此时 `bad_alloc`，批次已持久化而内存中的水位未前进，重试同一 `cmd_seq` 会被当作新命令**再次执行**——正是 M5 要防止的重复执行。
   修正：摘要与记录容量在**执行之前**准备好（失败即"未生效"）；提交点之后的步骤不分配、不抛出。M5 变异表中"提交记录在提交点后分配"
   由分配注入扫描检出——但这是在第二次修正之后：最初的扫描只注入第 0–7 次分配，而一次 `submit` 有多达 20 次分配（大部分在 `encode_batch`），
   从未到达提交点之后的代码，该变异**存活**；扫描改为对每条命令一直注入到不再命中为止（80 条命令共 989 个注入点）后才被杀死。
2. **中毒必须先置标志、后组装消息。** 初版先构造原因字符串再置位；若构造本身 `bad_alloc`，异常会在会话**未中毒**的情况下逃逸，
   调用方可能继续使用领先于日志的内存状态。修正后 `poison()` 为 `noexcept`，消息尽力而为。
3. **文本格式不能表达非法 `Side`。** M3/M4 生成器会产生二进制强转得到的非法侧向；写入命令文件前用 `testkit::text_representable`
   改写为价格 0 的订单（同样被拒、对状态影响相同），使生成器的模型保持对齐；只有拒单原因不同。`INVALID_SIDE` 因此只在内存接口中可测。
4. **重启时必须先截断残缺尾部再追加**（否则下一次恢复看到的是中间损坏）。`FileSink(path, valid_bytes)` 负责截断，并有直接的文件系统测试；
   崩溃测试中 40 次 `kill -9` 没有一次落在单次 `write` 内部（尾部均为 CLEAN），残缺尾部的处理由逐字节截断的单元测试覆盖。

