# DECISIONS

每项决策记录：背景 → 选项 → 选择 → 代价 → 复审条件。状态：**已采纳** / **暂定**（待后续模块确认） / **已废弃**。

---

### D-001 价格用 `int64` 整数 tick — 已采纳（M1）

- 选项：`double`；定点十进制类；`int64` tick。
- 选择：`int64` tick。浮点比较与累加不满足确定性和精确相等；十进制类对单标的无收益。
- 代价：输入层需负责 tick 换算（本项目输入直接以 tick 给出）。

### D-002 基本类型用别名而非强类型包装 — 暂定（M1，M3 后复审）

- 选项：A `using Price = int64_t`；B `StrongInt<Tag>` 包装，禁止 Price/Quantity 混用。
- 选择：A。命令是聚合体，统一用 C++20 指派初始化（`NewOrder{.id=…, .price=…}`）构造，
  参数错位这一主要风险在构造点即被字段名消除；B 会让撮合代码充满 `.value()`，可读性下降。
- 代价：库内部函数若以位置参数同时接收价格和数量，编译器无法发现错位。
- 复审条件：M2/M3 中出现 ≥ 2 个同时接收 `Price, Quantity` 位置参数的函数，或测试发现一次此类错误。

### D-003 数量用有符号 `int64` — 已采纳（M1）

- `uint64` 下 `remaining - fill` 的错误会静默回绕成巨大正数；`int64` 下负数可被断言/校验发现，
  且 UBSan 能捕获有符号溢出。合法域靠校验限定为 `(0, max]`。

### D-004 错误处理：运行期输入错误用拒单码，配置错误用异常 — 已采纳（M1）

- 外部输入非法是常态，不是异常：返回 `std::optional<RejectReason>`，热路径无异常、无分配。
- 配置非法是部署错误，构造时抛 `std::invalid_argument` 立即失败。
- 内部不变量破坏是程序错误：测试中由 `check_invariants()` 检出（Release 中也可调用，不依赖 `assert`）。
- 未使用 `std::expected`：属于 C++23，项目标准为 C++20。

### D-005 校验函数 `constexpr` 内联于头文件 — 已采纳（M1）

- 校验在每条命令的热路径上，放在 `.cpp` 中跨编译单元无法内联（未启用 LTO 时）。
- `constexpr` 使边界用例可用 `static_assert` 在编译期验证。

### D-006 配置约束保证合计不溢出 — 已采纳（M1）

- 引入 `max_live_orders` 并要求 `max_live_orders × max_order_quantity ≤ INT64_MAX`、
  `max_price × max_order_quantity ≤ INT64_MAX`。
- 备选：每次累加都做溢出检查并在溢出时……无法合理处理（拒单会让结果依赖历史累计量，难以解释）。
- 代价：多一个配置项和一个拒单原因 `BOOK_FULL`；它同时也是有现实意义的风控上限（限制内存）。

### D-007 订单簿基线：`std::map` 价格层 + `std::list` FIFO + `unordered_map` 索引 — 暂定（M2 确认）

- 先用标准容器建立可证明正确的基线，侵入式链表/对象池/扁平价格数组作为 Level 2 的
  有假设实验，而不是预先引入。见 DESIGN §2。

### D-008 重复 ID 仅与活跃订单比较 — 暂定（M3 确认）

- 选项：A 与活跃订单冲突才拒绝；B 会话内任何出现过的 ID 都拒绝（需全量集合，内存随历史增长）；
  C 要求 ID 严格递增（O(1)，蕴含 B）。
- 倾向 A：O(1) 且无额外状态，与多数交易所"活跃订单内唯一"的约束一致。
- 代价/风险：客户端对已终结订单发出的迟到撤单，可能命中复用该 ID 的新订单。
  在本项目中由输入方保证不复用；C 作为后续修改任务候选。

### D-009 价格下界 `min_price ≥ 1` — 已采纳（M1）

- 负价格（价差合约等）不在单标的现货/期货场景内；正价格让"0 = 缺省/未填"可被校验拒绝。
- 撮合逻辑只做比较，放开这一约束不影响撮合代码，只需改 `validate_config` 与测试。

### D-010 构建与依赖 — 已采纳（M0）

- WSL2 Ubuntu 24.04；GCC 13.3 / Clang 18.1；CMake 3.28 + Ninja。
- GoogleTest：优先 `find_package(GTest)`（Ubuntu `libgtest-dev` 1.14），否则 `FetchContent` 固定 `v1.14.0`。
- 警告全开且 `-Werror`；Sanitizer 构建为 Debug + `-fsanitize=address,undefined -fno-sanitize-recover=all`，
  保证断言开启且 UB 致命。
- Debug 构建定义 `_GLIBCXX_ASSERTIONS`（标准库边界检查）。

### D-011 模块 = 独立 CMake 目标 + 独立测试可执行文件 — 已采纳（M0）

- 让依赖方向在链接层面可见；单个模块可单独构建、测试与审阅。
- 代价：头文件仍在同一 `include/` 根下，`#include` 层面的越层引用不会被编译器阻止；靠代码审查约束。
