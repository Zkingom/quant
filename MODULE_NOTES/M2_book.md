# M2 book —— 技术说明

状态：✅ 实现完成，2026-10-20 在 6 个预设下验收通过（证据见 §8 与 [evidence/M2/](evidence/M2/)）。

## 1. 职责与边界

**负责**：按价格-时间优先存放挂单；按订单 ID 定位；对"某侧最优价最早一单"执行减量；撤单；
在任何操作之后保持结构一致，并能完整地自检这一点。

**不负责**：撮合决策（是否成交、成交多少、taker 的剩余如何处理）、有状态拒单（重复 ID / 未知 ID / 容量的**拒单事件**）、
事件生成、序号分配。这些都属于 M3。订单簿对"taker"一无所知。

**依赖**：M1（类型、`NewOrder`、`EngineConfig`、`validate_new_order`、`checked_add`）。
**被依赖**：M3 将是唯一的生产调用方；M5 快照将使用 `for_each_order`。

## 2. 文件组织

```
include/lob/book/order_book.hpp      OrderBook 与返回值类型（OrderView/Fill/CancelledOrder/LevelSummary）
src/book/order_book.cpp              全部非模板实现
tests/book/book_test_util.hpp        dump()（规范文本）、buy()/sell()、EXPECT_BOOK_CONSISTENT
tests/book/test_order_book.cpp       单元测试：语义、契约、容量、生命周期（23 个）
tests/book/test_order_book_tamper.cpp    Tamper Tests：每条不变量一个故意破坏（16 个）
tests/book/reference_book.hpp        朴素参考模型（仅测试用）
tests/book/test_order_book_differential.cpp  固定种子差分测试 + 参考模型自检（2 个）
tests/book/test_order_book_alloc_failure.cpp 分配失败注入，独立可执行文件（6 个）
examples/book_demo.cpp               可运行示例
scripts/mutation/m2_book.py          变异测试脚本
```

## 3. 实际数据结构

```
OrderBook
 ├─ config_    EngineConfig                                     构造时 validate_config()
 ├─ bids_      std::map<Price, PriceLevel, std::greater<>>      begin() = 最优买价
 ├─ asks_      std::map<Price, PriceLevel, std::less<>>         begin() = 最优卖价
 ├─ index_     std::unordered_map<OrderId, Locator>
 └─ last_seq_  SeqNum                                           最近一次成功 add 的 seq

PriceLevel   { Quantity total_quantity; std::list<RestingOrder> orders; }
RestingOrder { OrderId id; Quantity remaining; SeqNum seq; }
Locator      { Side side; Price price; PriceLevel* level; std::list<RestingOrder>::iterator order; }
```

几个刻意的选择：

- **`RestingOrder` 不存侧与价**：它们由所在层隐含，冗余存储只会多一个可能不一致的地方。
  需要完整信息时，查询返回自包含的 `OrderView` 副本，调用方拿不到内部引用。
- **`Locator` 存指针而非 map 迭代器**：两侧比较器类型不同导致迭代器类型不同（D-014）。
- **`visit_side`**（[order_book.hpp](../include/lob/book/order_book.hpp) 私有模板）：用泛型 lambda 把"对哪一侧的 map 做什么"写一次，
  两侧共享同一份逻辑。这是消除重复的最小手段，不是新的抽象层。
- **`last_seq_`**：FIFO 的正确性依赖 seq 单调；订单簿不分配 seq（那是 M3 的事），但拒绝不单调的 seq，
  因此"队列按 seq 递增"由构造保证，而不是靠调用方自觉。

## 4. 接口契约

| 函数 | 前置条件（违反 → `std::logic_error`，簿不变） | 结果 | 异常保证 |
|---|---|---|---|
| `add(order, seq)` | ① 通过 `validate_new_order` ② ID 不在簿中 ③ `order_count() < max_live_orders` ④ `seq > last_seq_` ⑤ 买价 < 最优卖价 / 卖价 > 最优买价 | 入所在价格层队尾 | 强（含 `bad_alloc`） |
| `fill_front(side, qty)` | `is_valid(side)`；该侧非空；`0 < qty ≤ 队首.remaining` | `Fill{maker_id, price, qty, maker_remaining}`；减到 0 即出队，层空即删层 | 检查先于修改；成功路径不分配、不抛出 |
| `cancel(id)` | 无 | `CancelledOrder` 或 `nullopt` | `noexcept`，不分配 |
| 查询 | 带 `Side` 者要求 `is_valid(side)` | 值副本 | `depth` 可能 `bad_alloc`，其余 `noexcept` |

为什么"ID 不在簿中返回 `nullopt`"而"重复 ID 抛异常"不对称：撤单一个不存在的 ID 是**正常的外部输入**
（迟到撤单、重复撤单），订单簿据实回答；而挂一个已存在的 ID 只可能是 M3 漏做了重复检查——这是内部 bug（D-012）。

注意 ⑤ 包含**等价锁定**：买价 == 最优卖价也拒绝。因为这样的订单本应成交，挂上去意味着引擎漏撮合。

## 5. 关键函数实现逻辑

### 5.1 `add` → `insert`（[order_book.cpp:56](../src/book/order_book.cpp:56)、[order_book.cpp:86](../src/book/order_book.cpp:86)）

```
add(order, seq)
  ├─ ①–⑤ 前置检查（只读；任何一项失败 → contract_violation → throw，无修改）
  └─ visit_side(order.side, insert)
       insert(levels, order, seq)
         1. try_emplace(price)            可能分配 map 节点；失败 → 无副作用
         2. orders.push_back(...)         可能分配链表节点；失败 → 若步骤 1 新建了层则删除，重抛
         3. index_.emplace(id, Locator)   可能分配哈希节点/桶数组；失败 → pop_back + 删新层，重抛
         4. total_quantity += qty         不抛出；不溢出（D-006）
         5. last_seq_ = seq               不抛出
```

修改顺序的要点：**会失败的步骤在前，不会失败的步骤在后**；每个会失败的步骤只需撤销它之前的步骤。
步骤 4、5 放在最后，所以任何失败都不需要回滚计数。
`Locator.order = std::prev(level.orders.end())` 在步骤 2 成功后立即取得，此时它正是刚插入的节点。

### 5.2 `fill_front`（[order_book.cpp:119](../src/book/order_book.cpp:119)）

```
fill_front(side, qty)
  ├─ is_valid(side)? 否 → throw
  └─ visit_side(side, λ)
       levels 为空?              → throw
       level_it = levels.begin()   最优价
       maker   = level.orders.front()   最早一单
       qty ∉ (0, maker.remaining]? → throw          ← 到此为止无修改
       maker.remaining -= qty; level.total_quantity -= qty
       fill = {maker.id, price, qty, maker.remaining}   ← 在任何删除之前构造
       if maker.remaining == 0:
           index_.erase(maker.id)   此时 maker 仍有效（读它的 id）
           orders.pop_front()       maker 从此悬空
           if orders.empty(): levels.erase(level_it)   level 从此悬空
       return fill
```

易错点：`Fill` 必须在 `pop_front` 之前构造，`index_.erase` 必须在 `pop_front` 之前执行（它读 `maker.id`）。
注释标出了每个引用开始悬空的位置；ASan 构建会捕获任何越过这些点的访问。

### 5.3 `cancel`（[order_book.cpp:153](../src/book/order_book.cpp:153)）

`Locator` 先**按值拷贝**，因为随后要删除它所在的索引项；然后：减层总量 → 删链表节点 → 删索引项 → 若层空，按价格删层。
所有操作都不分配内存，函数声明为 `noexcept`；分配注入测试在"首次分配即失败"的条件下验证了这一点（分配计数 0）。

### 5.4 `find_invariant_violation`（[order_book.cpp:237](../src/book/order_book.cpp:237)）

逐侧、逐层、逐单遍历；对每一单通过 `index_.find(id)` 取得索引项，比较**侧、价、层指针、节点地址**四项。
由于每个索引项指向唯一的节点地址，遍历中匹配到的索引项两两不同；再加上"匹配数 = 索引项总数"，即得双射。
求和用 `checked_add`：损坏状态下（订单数可能超过配置上限）也不会触发有符号溢出 UB——这条分支本身有 Tamper Test 覆盖。

### 5.5 移动操作（[order_book.cpp:26](../src/book/order_book.cpp:26)、[order_book.cpp:38](../src/book/order_book.cpp:38)）

见 §6。

## 6. 对象生命周期

| 阶段 | 行为 | 依据 |
|---|---|---|
| 构造 | 拷贝并校验配置；非法配置抛 `std::invalid_argument` | M1 `validate_config` |
| 拷贝 | **禁止**：逐成员拷贝后新对象的 `Locator` 会指向源对象的节点 | `static_assert(!is_copy_constructible_v)` |
| 移动构造 | 容器移动转移节点所有权，节点地址不变 → 所有 `Locator` 仍有效、改归目标对象；随后清空源对象三个容器 | D-013 |
| 移动赋值 | 先销毁目标原有内容（容器移动赋值），再同上；自赋值为空操作；配置随内容一起转移 | D-013 |
| 被移动后的源对象 | 空、一致、保留原配置与 `last_seq_`，可继续正常使用且与目标对象完全独立 | 测试 `MovedFromBookIsEmptyConsistentAndReusable` |
| 析构 | 默认；容器自行释放节点 | — |

"`Locator` 在移动后指向目标对象的节点"由不变量 4 验证：检查器遍历的是**目标对象自己的** map，
并比较每个 `Locator` 中的层指针与节点地址，所以若索引仍指向源对象，检查会立即失败。

已知限制：变异"移动构造不清空源"在本工具链上**存活**（等价变异，见 §8.4 与 D-013）。

## 7. 一个完整的调用链案例（`examples/book_demo` 实际输出摘录）

初始：6 次 `add` 之后

```
          103 |    12  (1 orders)          asks: 102:[#4 x8, #6 x4]  103:[#5 x12]
          102 |    12  (2 orders)
  ------------------------
     15 |    100          (2 orders)        bids: 100:[#1 x10, #2 x5]  99:[#3 x20]
     20 |     99          (1 orders)
```

模拟"一个卖 12 的 taker"（M3 将如此调用）：

1. `fill_front(Buy, 10)`：`bids_.begin()` = 100 → 队首 #1，剩 10 → 合法 → #1 剩 0、层总量 15→5 →
   返回 `Fill{1,100,10,0}` → 删索引 #1 → `pop_front` → 层 100 仍非空，保留。输出 `fill maker=1 price=100 qty=10 maker_remaining=0`。
2. `fill_front(Buy, 2)`：队首现为 #2（时间优先），5→3，层总量 5→3，返回 `Fill{2,100,2,3}`，不删任何东西。
3. `cancel(4)`：索引 → `{Sell,102,&层102,→#4}` → 层总量 12→4 → 删节点 → 删索引 → 层 102 剩 #6，保留。
   再次 `cancel(4)` → `nullopt`（输出 `not resting`）。
4. `add(buy #7 @102)`：前置检查 ⑤ 发现 102 ≥ 最优卖价 102 → `logic_error`
   `OrderBook::add: order at 102 would cross the opposite best 102`；随后打印的盘口与之前完全相同，不变量 `OK`。

完整输出：[evidence/M2/book_demo.out](evidence/M2/book_demo.out)。

## 8. 验证证据（2026-10-20 实际执行）

环境：[evidence/M2/environment.txt](evidence/M2/environment.txt)（WSL2 Ubuntu 24.04.1、GCC 13.3.0、Clang 18.1.3、CMake 3.28.3、Ninja 1.11.1）。

### 8.1 六预设

`scripts/check.sh`，从删除 `build/` 后的全新构建开始：六个预设全部 `[PASS]`，每个 76/76（M1 29 + M2 47），
`-Werror` 下零警告。完整日志：[evidence/M2/check_all_presets.log](evidence/M2/check_all_presets.log)。

### 8.2 差分测试（20 个种子 × 5,000 步 = 100,000 次操作，每步全状态比较）

```
adds=28056 new_level=11865 reused_dead_id=3920
rejected: crossing=1698 duplicate=2188 capacity=9617 invalid=1753 stale_seq=1795
cancels hit=15730 miss=9279; fills partial=16147 complete=11571 rejected=2142
levels_removed=11512 max_orders=40
```

每步比较：不变量、订单数、两侧完整优先级序列（含 seq）、两侧深度、层数、最优价、队首、每层 `quantity_at`、每单 `find`。
测试还断言上述每个计数器 > 0、且达到容量上限 40，防止生成器退化后测试悄悄变弱。
上面的计数在 gcc-debug、gcc-asan、clang-release 上**完全相同**（[evidence/M2/differential_and_alloc.log](evidence/M2/differential_and_alloc.log)），
说明生成器在编译器与优化级别之间是确定性的（两者均使用 libstdc++；未在 libc++ 上验证）。

### 8.3 分配失败注入

| 场景 | 结果 |
|---|---|
| 空簿首单（新层） | 精确 4 次分配（map 节点、链表节点、哈希节点、首个桶数组），4 个注入点全部满足强保证 |
| 已有价格层 | 精确 2 次分配，2 个注入点全部满足强保证 |
| 0..64 个订单规模扫描 | 共 158 个注入点，单次 `add` 最多 4 次（含 rehash）；每点：簿逐字节不变、不变量成立、随后同一操作成功 |
| `fill_front`（部分 / 完全且保留层 / 完全且删层）、`cancel`（队首 / 删层 / 未知 ID） | "首次分配即失败"下全部成功，分配计数 0 |
| 契约违反时内存不足 | 构造异常消息时分配失败 → 得到 `bad_alloc` 而非 `logic_error`，但簿不变 |

安全性：注入只在独立可执行文件 `lob_book_alloc_tests` 内、只在 RAII 窗口内生效，每窗口至多一次失败；
GCC/Clang 的 ASan 构建中同样通过（修复 nothrow 形式后，见 D-016）。

### 8.4 变异测试（`scripts/mutation/m2_book.py`，[evidence/M2/mutations.log](evidence/M2/mutations.log)）

| 注入的缺陷 | 结果 | 检出者 |
|---|---|---|
| 新单入队首（破坏 FIFO） | 杀死 | 24 个测试 |
| 撤单后不删空层 | 杀死 | 7 个 |
| `fill_front` 不减层总量 | 杀死 | 单元 + 差分 |
| 交叉检查 `>=` 改 `>`（允许锁定） | 杀死 | 契约测试 + 差分 |
| `fill_front` 拒绝恰好完全成交 | 杀死 | 7 个 |
| 回滚：链表插入失败后不删新层 | 杀死 | **仅分配注入测试** |
| 回滚：索引插入失败后不 `pop_back` | 杀死 | **仅分配注入测试** |
| 移动构造不清空源对象 | **存活** | 等价变异：libstdc++ 的容器移动本就清空源（D-013） |
| `depth` 多返回一层 | 杀死 | 单元测试 |
| 检查器跳过 FIFO seq 检查 | 杀死 | Tamper Test |

9/10 被杀死；两个回滚缺陷只有分配注入测试能发现，说明这组测试不是摆设。

### 8.5 未验证项与风险

- **libc++ 未测试**：差分生成器的跨平台确定性、"被移动容器为空"的等价性都只在 libstdc++ 上观察过。
  复验：`cmake -S . -B build/libcxx -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-stdlib=libc++`（需安装 `libc++-dev libc++abi-dev`，以及用 libc++ 编译的 GTest——系统 GTest 链接 libstdc++，需走 `-DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON` 的回退路径）。
- **哈希函数抛异常**的路径未覆盖（`std::hash<uint64_t>` 不抛出，标准库保证下该路径不存在）。
- **对齐 `operator new`** 未纳入注入（簿中无过对齐类型）。
- 变异测试为 10 例人工设计的缺陷，不是系统性变异工具的结果；未覆盖查询函数中的所有比较运算。
- 性能未测量：每次 `add` 2–4 次分配是结构性事实，其时间代价留待 M6/L2。

## 9. 易错点与边界条件

| 情形 | 行为 | 测试 |
|---|---|---|
| 买价 == 最优卖价 | 拒绝（锁定也算交叉） | `AddRejectsCrossingOrLockingPrice` |
| 价格带两端 | 可挂单 | `PriceBandEdgesAreUsable` |
| seq 有间隙 | 允许（只要求严格递增） | `AddRejectsNonIncreasingSeq` |
| seq = 0 | 拒绝（`last_seq_` 初值 0） | 同上 |
| 已撤/已成交 ID 再挂 | 允许（D-008） | `IdMayBeReusedOnceNoLongerResting` |
| 容量满后撤一单 | 可再挂 | `AddRejectsWhenFullAndRecoversAfterRemoval` |
| 部分成交后撤单 | 返回剩余而非原始数量 | `CancelReturnsRemainingAfterPartialFill` |
| 部分成交 | 保持队列位置 | `PartialFillKeepsQueuePosition` |
| `depth(side, 0)` | 空结果 | `BestPricesFollowPricePriority` |
| 非法 `Side` 传给查询 | **未定义为错误**：按卖方处理（前置条件违反，不检查） | 未测（契约外） |

## 10. 修改任务（供独立练习；只给需求与验收，不给实现）

**T2-1 保持队列位置的减量（amend down）。** 新增 `reduce(OrderId id, Quantity new_remaining)`：
把挂单剩余量减少到 `new_remaining`，保持其队列位置（真实交易所的常见规则：减量不丢优先级，增量要丢）。
验收：减量后该单仍在同一位置、层总量同步；`new_remaining` 不在 `(0, 当前剩余)` 内时为契约违反且簿不变；
ID 不存在返回"未找到"（自行设计返回类型并说明理由）；分配注入测试断言其分配次数为 0；
参考模型实现同一操作，差分测试加入该操作并增加对应覆盖计数器（> 0）。

**T2-2 一侧总量。** 新增 `side_quantity(Side)`，O(1) 返回一侧全部挂单剩余量之和（增量维护，不得遍历）。
验收：不变量检查增加该项，并配一个 Tamper Test；差分测试每步与参考模型的线性求和比较；
在 MODULE_NOTES 中论证为什么它不会溢出（引用配置约束）。

**T2-3 撤单清空层的 O(1)。** 让"撤掉层内最后一单"不再需要按价格 `erase(key)`（当前 O(log L)，见 D-014），
同时不得引入运行期比较器分支。
验收：现有 76 个测试全部通过；分配次数断言不变；在 DECISIONS 中新增条目比较方案前后的代码复杂度，
并说明在没有基准数据前是否值得合入（允许结论为"不合入"）。

**T2-4 契约检查的编译期开关。** 增加 CMake 选项（默认开启）在构建期关闭 `OrderBook` 的契约检查，作为 L2 实验的准备。
验收：开启时行为不变；关闭时，契约类测试以 `GTEST_SKIP` 明确跳过而不是失败，其余测试全部通过；
`scripts/check.sh` 的六个预设仍使用默认（开启）；文档写明关闭后哪些保证不再成立。

**T2-5 差分测试中途移动。** 在差分驱动中每隔随机步数把簿移动构造到新对象、再随机地移动赋值回来，然后继续运行。
验收：同样 20 个种子全部通过；新增"移动次数"覆盖计数器且 > 0；被移动后的源对象在每次移动后都通过不变量检查且为空。

## 11. 总结

- **已验证能力**：价格优先、同价 FIFO、部分/完全成交、跨层（层清空后次优层接替）、撤单（各队列位置、部分成交后、重复、未知、已成交）、
  ID 复用、容量上限、交叉/锁定拒绝、seq 单调；六条结构不变量及其检出能力；`add` 的强异常保证（每个分配点）；
  `fill_front`/`cancel` 零分配；移动后索引正确、源对象空且可用；与独立参考模型在 10 万次随机操作上逐步一致；
  六预设零警告通过。
- **遗留问题**：见 §8.5；`Side` 非法值在查询中不检查；撤单清空层为 O(log L)。
- **对 M3 的接口承诺**：撮合循环 = `front(对手侧)` → 判断价格是否可成交 → `fill_front(对手侧, min(taker 剩余, maker 剩余))`，
  直到不可成交或对手侧为空；剩余量通过 `add` 挂单。M3 必须在调用前把重复 ID、容量、非法输入转换为拒单事件——
  订单簿对这些情形只抛 `logic_error`。
