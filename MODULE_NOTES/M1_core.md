# M1 core —— 技术说明

状态：✅ 实现完成，2026-10-19 在 6 个预设下测试通过（见 §6）。

## 1. 职责与边界

**负责**：全项目共享的词汇表与一切"只看命令本身 + 静态配置就能判定"的规则。

- 值类型：`Price`、`Quantity`、`OrderId`、`SeqNum`、`Side`
- 命令：`NewOrder`、`CancelOrder`、`Command = std::variant<…>`
- 拒单原因：`RejectReason` 及其稳定字符串
- 配置：`EngineConfig` 与 `validate_config()`
- 无状态校验：`validate_new_order()`、`validate_cancel_order()`
- 溢出检查算术：`checked_add/sub/mul`

**不负责**：任何依赖订单簿状态的判断（重复 ID、未知 ID、容量）—— 这些属于 M3。
M1 只为它们定义了拒单码，使拒单词汇表集中在一处。

**依赖**：仅 C++20 标准库。被所有后续模块依赖，因此接口变动代价最高，刻意保持最小。

## 2. 文件组织

```
include/lob/core/
  types.hpp           基本类型、Side、is_valid/opposite/to_string
  checked.hpp         checked_add / checked_sub / checked_mul
  command.hpp         NewOrder、CancelOrder、Command
  reject_reason.hpp   RejectReason + to_string
  config.hpp          EngineConfig + validate_config 声明
  validation.hpp      validate_new_order / validate_cancel_order（constexpr）
src/core/config.cpp   validate_config 实现（唯一的非内联代码）
tests/core/           test_types / test_checked / test_config / test_validation
examples/core_validation_demo.cpp
```

为什么几乎全在头文件里：校验位于每条命令的热路径，放在 `.cpp` 中未开 LTO 时无法跨单元内联（D-005）；
`constexpr` 还允许把边界用例写成 `static_assert`，编译即验证。`validate_config` 只在启动时调用一次且会抛异常，
放到 `.cpp` 中既不影响性能，又能把 `<stdexcept>` 挡在公共头文件外。

## 3. 关键设计与成立理由

### 3.1 无效默认值

`NewOrder{}` 的 `id=0, price=0, quantity=0` 都是校验必拒的值。漏写字段的命令不可能变成合法订单。
`Side` 无法有"无效默认值"（只有两个枚举量），所以漏写 side 会得到 `Buy` —— 但同时漏写的其余字段仍保证拒单；
单独漏写 side 而其他字段完整的情况无法在类型层面防止，这是已知限制。

### 3.2 `Side` 的非法值

`enum class Side : std::uint8_t` 有固定底层类型，因此 `static_cast<Side>(7)` 是**合法且已定义**的值
（C++ [dcl.enum]/8），不是 UB。来自二进制解码或强转的 `Side` 必须经过 `is_valid()`。
测试 `Side.OnlyTwoUnderlyingValuesAreValid` 穷举了全部 256 个底层值。

`opposite()` 对非法值返回 `Buy`（前置条件违反时不报错）。之所以可接受：它只会在通过校验的订单上被调用。

### 3.3 校验顺序是协议的一部分

当多个字段同时非法时，返回的原因会写入事件流和 Golden 文件，因此顺序必须固定：
`id → side → price → quantity>0 → quantity≤max`。测试 `ReportsFirstViolationInDocumentedOrder`
从"全部非法"开始按顺序逐个修正字段，断言每一步报告的原因随之推进。

### 3.4 配置约束把溢出问题变成定理

`max_price × max_order_quantity ≤ INT64_MAX` 保证单笔名义金额可表示；
`max_live_orders × max_order_quantity ≤ INT64_MAX` 保证任意数量合计可表示（M3 用 `BOOK_FULL` 强制活跃订单上限）。
于是后续模块中的价格层总量累加**不需要**逐次溢出检查，只需在调试/测试时由不变量检查兜底。
边界测试利用 `INT64_MAX = 49 × 188232082384791343` 精确命中上限（积恰为 `INT64_MAX` 通过，+1 拒绝）。

### 3.5 `checked.hpp`

`__builtin_*_overflow` 对所有整数类型精确、可在常量表达式中使用、在 GCC/Clang 上编译为一条运算 + 溢出标志检查。
排除 `bool`（内建函数不接受）。非 GCC/Clang 编译器直接 `#error`，而不是退化为不精确实现。

## 4. 调用链与具体案例

运行期调用链（M3 实现后的预期用法；目前由测试与示例直接调用）：

```
MatchingEngine::process(Command)                 [M3, 计划]
  └─ std::visit → NewOrder 分支
       └─ validate_new_order(order, config_)     [M1]
            ├─ id == 0 ?           → InvalidOrderId
            ├─ !is_valid(side) ?   → InvalidSide
            ├─ price 不在价格带 ?  → PriceOutOfRange
            ├─ quantity <= 0 ?     → InvalidQuantity
            ├─ quantity > max ?    → QuantityTooLarge
            └─ nullopt → 进入有状态校验与撮合 [M3]
```

具体输入（`examples/core_validation_demo`，配置：价格带 `[100,200]`，最大数量 1000），实际输出：

```
config: price band [100, 200], max qty 1000
id=1 side=BUY price=150 qty=10 -> OK
id=2 side=SELL price=200 qty=1000 -> OK            ← 两个上界都包含
id=0 side=BUY price=150 qty=10 -> INVALID_ORDER_ID
id=3 side=INVALID price=150 qty=10 -> INVALID_SIDE ← static_cast<Side>(7)
id=4 side=BUY price=99 qty=10 -> PRICE_OUT_OF_RANGE
id=5 side=SELL price=150 qty=0 -> INVALID_QUANTITY
id=6 side=SELL price=150 qty=1001 -> QUANTITY_TOO_LARGE
```

## 5. 不变量与易错点

| 项 | 说明 |
|---|---|
| 价格带闭区间 | `min ≤ price ≤ max`，两端都合法。测试覆盖 `min-1/min/min+1/max-1/max/max+1`。 |
| 数量 0 | 属于 `INVALID_QUANTITY`，不是 `QUANTITY_TOO_LARGE`；负数同理。 |
| `INT64_MIN` | 作为价格/数量输入时只做比较、不做取负或减法，因此不会触发溢出。 |
| 拒单字符串 | 被 Golden 文件依赖，只能新增、不能改拼写。`to_string` 的 `switch` 无 `default`，配合 `-Wswitch -Werror` 新增枚举量忘写字符串会编译失败。 |
| 配置前置条件 | `validate_new_order` 假设配置已校验；对未校验的倒置价格带它会拒绝一切价格，不会崩溃，但这不是受支持的用法。 |

## 6. 验证证据（2026-10-19 实际执行）

环境：WSL2 Ubuntu 24.04.1，内核 6.6.87.2-microsoft-standard-WSL2，GCC 13.3.0，Clang 18.1.3，CMake 3.28.3，Ninja 1.11.1，GoogleTest 1.14（系统包）。

| 检查 | 命令 | 结果 |
|---|---|---|
| 六预设构建 + 测试 | `scripts/check.sh` | 全部 `[PASS]`；每个预设 29/29 测试通过；`-Werror` 下零警告 |
| Sanitizer 确实生效 | 检查 `build/*-asan/compile_commands.json` | asan 预设 6/6 编译单元带 `-fsanitize=address,undefined`；Release 中为 0 |
| UBSan 致命性 | 用相同标志编译一个有符号溢出程序 | 报 `runtime error: signed integer overflow` 并以非零码退出 |
| 测试的检出能力（变异测试） | 手工注入 3 个差一错误后重跑 | `price >= max`：3 个测试失败；`quantity < 0`：3 个失败；`min_price < 0`：2 个失败 |
| GTest 回退包哈希 | `curl … v1.14.0.tar.gz \| sha256sum` | 与 `tests/CMakeLists.txt` 中 `URL_HASH` 一致 |
| GTest 回退路径 | `-DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON` 全新构建 | 从源码编译 GTest，29/29 通过 |

**未验证项**：
- 未运行 clang-tidy / clang-format（当前未配置）。
- 变异测试为手工抽样 3 例，非系统性变异测试工具的结果。

## 7. 修改任务（供独立练习；只给需求与验收，不给实现）

**T1-1 拒单原因的单一真源。** 增加 `constexpr std::array all_reject_reasons()`（或等价机制），
让 `RejectReason.StringsAreStableAndUnique` 遍历它而不是手写数量 8。
验收：新增一个枚举量但不更新该数组时，至少有一个测试或 `static_assert` 失败；现有 29 个测试仍通过。

**T1-2 最小交易单位（lot size）。** 在 `EngineConfig` 增加 `lot_size ≥ 1`，新增拒单原因 `QUANTITY_NOT_MULTIPLE_OF_LOT`，
插在校验顺序 `quantity ≤ max` 之后。
验收：`lot_size=1` 时行为与现在完全一致；`lot_size=10` 时 `quantity=10/20` 通过、`15` 拒绝、`1001`（>max）仍报 `QUANTITY_TOO_LARGE`；
`validate_config` 拒绝 `lot_size ≤ 0` 以及 `lot_size > max_order_quantity`；校验顺序测试相应扩展。

**T1-3 结构化配置错误。** 把 `validate_config` 的错误从字符串改为 `enum class ConfigError` + 异常携带该枚举（或返回 `std::optional<ConfigError>`），
测试改为比较枚举而不是子串匹配。
验收：`test_config.cpp` 中不再出现 `.find(`；每条约束一个枚举值且有对应测试。

**T1-4 放开负价格（D-009 的反向实验）。** 允许 `min_price ≤ 0`，名义金额约束改为基于 `max(|min_price|, |max_price|)`。
验收：`min_price=-100, max_price=100` 通过且 `-100/0/100` 价格合法；`min_price=INT64_MIN` 被拒绝（取绝对值会溢出）；
`NewOrder{}` 的默认价格 0 在这种配置下不再必拒——需要在测试中显式记录这一语义变化。

**T1-5 强类型实验（D-002 的备选）。** 在独立分支上把 `Price`/`Quantity` 改为不可隐式互转的包装类型。
验收：`static_assert(!std::is_convertible_v<Quantity, Price>)` 成立；全部测试通过；
在 MODULE_NOTES 中记录改动行数和调用点可读性的前后对比，作为 D-002 复审输入。

## 8. 总结

- **已验证能力**：类型与命令定义；配置 6 条约束及其报告顺序；5 种无状态拒单及其顺序；溢出检查算术的边界；GCC/Clang × Debug/Release/Sanitizer 均通过。
- **遗留问题**：单独漏写 `side` 无法被类型系统发现；`opposite()` 对非法值静默返回；头文件层面的越层引用未被工具阻止（D-011）。
- **后续依赖**：M2 使用 `Price/Quantity/OrderId/SeqNum/Side` 与 `EngineConfig`；M3 使用命令、校验与全部拒单码，并负责 `DUPLICATE_ORDER_ID / UNKNOWN_ORDER_ID / BOOK_FULL` 的判定。
