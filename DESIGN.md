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

## 4. 快照 📝（M5）

规范文本：卖方价格升序、买方价格降序，层内按 FIFO 列出 `id remaining`；附 FNV-1a 64 位哈希。
