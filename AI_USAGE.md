# AI_USAGE

本项目在 AI 编码助手 Claude（Anthropic，经 Claude Code）的参与下开发。本文件按模块如实记录分工，随每个模块更新。

## 记录约定

- **AI 贡献**：AI 实际产出的内容。
- **人工审查**：项目作者实际做过的审查、修改、决策。由作者本人填写；未填写即表示尚未进行。
- 未经作者审查的内容，不应作为作者独立完成的成果对外表述。

## M0 工程骨架 + M1 core（2026-10-19）

**AI 贡献**
- 仓库审计（仓库为空，无可复用成果）；WSL 中安装 cmake、ninja-build、clang、libgtest-dev。
- 架构、模块划分、实施顺序与验收门槛（ARCHITECTURE / ROADMAP / DESIGN / DECISIONS）。
- 全部 CMake 配置、M1 全部头文件与源文件、29 个测试、示例程序、`scripts/check.sh`。
- 执行六预设构建与测试、Sanitizer 有效性检查、3 例手工变异测试，并撰写 MODULE_NOTES/M1_core.md。

**人工审查**（作者填写）
- [ ] 已通读 M1 代码
- [ ] 已在本机独立复现 `scripts/check.sh`
- [ ] 已完成至少一项 M1 修改任务（T1-x）：
- 修改/否决的设计决策：

## M2 book（2026-10-19 → 2026-10-20）

**AI 贡献**
- `OrderBook` 的接口、实现（`include/lob/book/order_book.hpp`、`src/book/order_book.cpp`）与全部测试：
  单元测试、Tamper Tests、参考模型与固定种子差分测试、分配失败注入测试；`book_demo`；M2 变异测试脚本。
- 执行六预设构建与测试、10 例变异测试，收集 `MODULE_NOTES/evidence/M2/` 中的日志；撰写 M2 模块说明并同步其余文档。
- AI 自查发现并修复的问题（均有记录）：
  1. 差分测试的"复用已失效 ID"计数器恒等于新增次数（统计口径错误，指标无效）——已修正；
  2. 生成器失衡（簿长期满载、九成撤单命中不存在的 ID）——调整操作比例与撤单选 ID 方式，未放宽任何断言；
  3. 分配注入桩未替换 nothrow 形式，ASan 下报 alloc-dealloc-mismatch——已修复；
  4. 文档中"depth 一次分配"的表述不实——已更正，并将两种 `add` 场景的分配次数改为精确断言。

**作者决策**
- 2026-10-20：采纳 D-002、D-007、D-008 的 AI 倾向方案，并要求 M2 完成后先交付报告、不自动进入 M3。

**人工审查**（作者填写）
- [ ] 已通读 M2 代码
- [ ] 已在本机独立复现 `scripts/check.sh` 与 `scripts/mutation/m2_book.py`
- [ ] 已完成至少一项 M2 修改任务（T2-x）：
- 修改/否决的设计决策：

## M3 engine（2026-10-20）

**AI 贡献**
- 先行设计（DESIGN §3：接口、事件模型、撮合语义、校验顺序、两阶段异常语义及其不可强保证的论证、不变量、测试设计），后实现。
- `MatchingEngine` 与事件类型的实现；M2 新增只读 `for_each_order_while`（D-018）。
- 测试：30 个单元测试、事件审计器及 15 个自检、参考引擎与 10 万条命令的差分测试、7 个分配失败注入测试；
  将 RNG 与分配注入桩抽到 `tests/support/` 供 M2/M3 共用；变异测试脚本拆为通用执行器 + 每模块表；`engine_demo`；`scripts/evidence_m3.sh`。
- 撰写 D-018 至 D-025、M3 模块说明，同步 DESIGN / ARCHITECTURE / ROADMAP / README。
- AI 自查发现并修复的问题：
  1. 一个单元测试的期望 seq 手算错误（`CancelRejections`），在首次构建前复核时修正；
  2. 差分生成器从未让簿达到容量上限，`BOOK_FULL` 与"满载时受理"零覆盖——由覆盖断言发现；加入建簿阶段，
     随后又修正了过度饱和（`BOOK_FULL` 占 38%）；
  3. 审计器的逐命令守恒检查在构造上必然成立，不是独立证据——改为针对引擎簿检查，并在文档中如实说明；
  4. 一个变异被编译器（`-Werror` 未使用变量）而非测试杀死——改写后重跑；
  5. 后台等待脚本用 `pgrep -f` 匹配到自身导致不退出——已终止，未影响结果。

**作者决策**
- 2026-10-20：批准进入 M3；要求先完成接口、语义、异常安全与测试设计再编码；M3 验收后停止，不自动进入 M4。

**人工审查**（作者填写）
- [ ] 已通读 M3 代码与 DESIGN §3
- [ ] 已独立复现 `scripts/evidence_m3.sh`
- [ ] 已完成至少一项 M3 修改任务（T3-x）：
- 修改/否决的设计决策：

## M4 testkit（2026-10-20）

**AI 贡献**
- 审计现有测试基础设施；设计并实现 `lob::testkit`（参考引擎、事件审计器、工作负载画像与生成器、差分执行器、分配故障注入、`lob_soak`）；
  将 M2/M3 测试迁移到 testkit；10 个工作负载画像及其边界目标；长序列故障注入；M5 契约测试（C1–C6）；
  `scripts/soak.sh`、`scripts/evidence_m4.sh`；D-026 至 D-033；M4 模块说明；同步 DESIGN / ARCHITECTURE / ROADMAP / README。
- 生产代码（`include/`、`src/`）在 M4 中**零改动**。
- AI 自查发现并修复的问题：
  1. 审计器的累计数量为 `int64`，长跑中会在测试预言机内部溢出（UB）——改为 128 位，并用 `extreme_quantity` 画像实际越过 `INT64_MAX`；
  2. 4 个初版画像未到达其声称的边界（深簿档位不足、单价位队列像随机游走、薄簿漂移到价格带边缘、极端数量分散在多个档位）——逐一分析原因并修改画像；
  3. 档位指标只在全量检查时采样，漏掉短暂峰值——改为每条受理命令后采样最优档；
  4. 一个先验目标（"成交数 ≥ 受理数"）处于稳态上限附近、且与另一目标冲突——给出结构性论证后替换，并拆分画像；
  5. 两处 shell heredoc 把 `\n` 转义写成真实换行（编译失败）——改用文件编辑工具写含转义的代码；
  6. 变异执行器的源码复制列表漏掉新增的 `testkit/`，变异运行在 CMake 配置阶段就失败；证据脚本未检查变异结果，仍报告成功——
     修复执行器，并让证据脚本在 check / soak / 变异任一阶段失败或未完成时退出非零；变异日志为修复后重跑所得（MODULE_NOTES/M4 §9.4）。

**作者决策**
- 2026-10-20：批准进入 M4；要求不以步数/测试数/覆盖率为成功指标，不改生产接口；M4 验收后停止，并列出 M5 必须解决的契约。

**人工审查**（作者填写）
- [ ] 已通读 testkit 与 M5 契约测试
- [ ] 已独立复现 `scripts/evidence_m4.sh`
- [ ] 已审阅画像目标与校准记录（MODULE_NOTES/M4 §6）
- 修改/否决的设计决策：

## M5 replay / snapshot / recovery（2026-10-20 → 2026-10-21）

**AI 贡献**
- 编码前在 DESIGN §6 写出：三种标识的关系、8 个特别审查问题的回答、文本格式 v1、会话状态机与提交协议、恢复流程、保证边界。
- 生产代码：引擎快照导出/恢复（M3 唯一新增接口）；`lob::io`（文本格式、日志、快照文件、恢复、会话、verify、文件 I/O）；`lob_replay`。
- 测试：格式、日志（逐字节截断、单字节修改）、快照（10 画像 × 多长度 × 多切点的后续行为一致性）、恢复、会话（写失败扫描、崩溃模型）、
  Golden（5 个手写 + 1 个固定种子）、分配注入下的 C1–C4 验收、文件系统、CLI Golden、`kill -9` 崩溃重启；M5 变异表；`lob_gen_commands`；`scripts/evidence_m5.sh`。
- 撰写 D-034 至 D-041、M5 模块说明，同步 DESIGN / ARCHITECTURE / ROADMAP / README。
- AI 自查发现并修复的问题：
  1. **会话在提交点之后分配内存**（命令摘要、提交记录）：若此时内存不足，已持久化的命令会在重试时再次执行——改为执行前准备；
  2. **中毒标志晚于消息构造**：消息构造失败时异常会在未中毒的情况下逃逸——改为先置标志、`noexcept`；
  3. 生成器产生的非法 `Side` 无法写入文本格式，固定种子 Golden 首次失败——新增 `text_representable`，并在文档中说明 `INVALID_SIDE` 只在内存接口可测；
  4. 崩溃测试首次失败：进程在写入任何内容前被杀死，脚本把"无日志"当作错误——这是合法的崩溃点，脚本改为接受；
  5. `FileSink` 的截断只被间接覆盖（崩溃测试中尾部均为 CLEAN）——新增直接的文件系统测试与对应变异；
  6. 变异执行器的复制列表预先加入新增的 `apps/` 目录（M4 教训）；`verify` 从 CLI 移入 `lob::io` 以便单元测试；
  7. shell heredoc 再次把 `\n` 写成真实换行——用编辑器修正；
  8. 变异测试发现分配注入扫描太浅（只注入前 8 次分配），"提交点后分配"的缺陷回退后没有任何测试失败——扫描改为覆盖每个分配点；
  9. 40 次 kill 的崩溃测试只有 8 次真正落在运行中（4000 条命令在 8 次重启后就跑完了）——命令数参数化，证据运行用 30000 条；
  10. Python 在 Windows 上以 CRLF 写文件，且一处文本被写入了真实的回车符——工作区统一为 LF，并记录为长期注意事项。
- 发布：按作者要求推送到 GitHub；推送前把提交作者邮箱改写为 GitHub noreply 地址（记录见 MODULE_NOTES/evidence/HISTORY_REWRITE.md）。

**作者决策**
- 2026-10-20：批准 M4、进入 M5；要求编码前完成特别审查；不得声称 exactly-once 或掉电持久性；M5 验收后停止。

**人工审查**（作者填写）
- [ ] 已通读 DESIGN §6 与 `src/io/session.cpp`
- [ ] 已独立复现 `scripts/evidence_m5.sh`
- [ ] 已完成至少一项 M5 修改任务（T5-x）：
- 修改/否决的设计决策：

## M6 benchmark / profiling / performance（2026-10-21 → 2026-10-22）

**AI 贡献**
- 测量前写定 BENCHMARK §1–§6（环境审计、四层场景、协议、开销归属、固定参数、profiling 方案）；冻结基线标签 `m6-baseline`。
- 测量工具：`bench/`（Book / Engine / Replay / Session 四层，吞吐与逐操作延迟分开测、精确分位数、状态哈希校验）；
  `scripts/bench.sh`、`bench_compare.sh`（两个修订、独立 worktree、交替运行）、`bench_env_compare.sh`、`bench_report.py`、`profile.sh`、`evidence_m6.sh`。
- 基线（gcc-release，5 个进程）、profiling（perf cpu-clock、callgrind、callgrind 缓存/分支模拟、perf trace）、
  实验 E1（Session 提交记录几何扩容）、探针 P1（glibc tcache）、放弃候选的记录；D-042 至 D-044；M6 模块说明与修改任务 T6-1 至 T6-8。
- AI 自查发现并修复的问题：
  1. **M5 中 AI 自己写入的 O(n²)**：`committed_.reserve(size + 1)`。M3 已在 D-022 记录过同一错误，M5 没有沿用；
     正确性测试的流太短，只有基准能发现。修正为 E1，并补上不依赖时间的分配字节数测试与变异；
  2. callgrind 的 `--toggle-collect=…process*` 同时匹配了嵌套重载，进入内层时把收集关掉，第一份引擎 profile 无效
     （每条命令只有 98 Ir、有 free 无 malloc）——发现后删除、改为只匹配 variant 重载并重做；
  3. 为了让 profiler 下的过滤运行不执行多余会话，给 `run_io` 加了按前缀的提前返回，但 `selected()` 是"过滤串是场景名的子串"，
     结果 `--filter session.ext4_fsync` 什么都不跑——`perf trace` 只看到 1 次 `write` 时发现，改为按完整场景名判断；
  4. WSL 会重启空闲的发行版并清空 `/tmp`，profiling 构建与对照 worktree 中途丢失——改放 `$HOME`；
  5. 整进程 `perf` 采样对引擎场景无效（样本被测试工具的生成阶段占据，DWARF 帧不一致）——如实记录为不可用，改用 callgrind；
  6. shell heredoc 再次把 `\n` 写成真实换行（新测试的 `printf`）——用编辑器修正；
  7. 文档 D-037 写的提交记录大小（24 字节）与实际（32 字节，含对齐）不符——在 perf 的复制步长中发现并更正。
  8. 第一次完整回归的变异阶段配置失败：变异执行器的复制列表没有新增的 `bench/`（M4 `testkit/`、M5 `apps/` 之后第三次同类问题）——补上后整套回归重新运行。
- 没有做到的：硬件计数器（环境不提供）；最大延迟尖峰的归因（T6-8）；裸机对照。

**作者决策**
- 2026-10-21：批准 M6；要求先测量后优化、不弱化任何语义与测试、不使用"超低延迟""生产级 HFT"等表述、M6 验收后停止，不进入 TCP/epoll。
- 2026-10-21："可否加速"——过滤对照场景、在基线后立即 profiling、把慢的 fsync 场景只放在与 E1 相关的对照中。
- 2026-10-22：M6 验收通过，合并 `m6-perf` 到 `main`（暂不推送）。
- 2026-10-22：E1 暂定保留；P1 作为"未检出显著收益"的有效实验记录；不追加新的优化候选；15 ms 最大延迟只做有限诊断；严格区分 Session 修复与引擎性能；完成 M6 后停止。

**人工审查**（作者填写）
- [ ] 已通读 BENCHMARK.md 与 MODULE_NOTES/M6_performance.md
- [ ] 已独立复现一次 `scripts/bench_compare.sh m6-perf~3 m6-perf 3 …`（或等价的 E1 前后对照）
- [ ] 已完成至少一项 M6 修改任务（T6-x）：
- 修改/否决的设计决策：

## L1 冻结评审（2026-10-24）

**AI 贡献**
- 按 ROADMAP §2.1 逐项核对 L1 门槛：每项对应到具名测试或证据文件，写出 `MODULE_NOTES/L1_REVIEW.md` 与缺口清单 G1–G6；评审本身不改代码与测试。
- 按作者决定只在文档中处理缺口：差分门槛的证据来源、Golden 大场景规模、接口命名、ARCHITECTURE 数据流与 M6 小节、ROADMAP 日期与 L2 现状；
  评审中另发现 ARCHITECTURE 指向已删除的 `tests/support/`，一并更正。

**作者决策**
- 2026-10-24：G1 以 `soak.sh` / `evidence_m6.sh` 为差分门槛的证据来源；G6 接受 2 000 条；修正 G2–G5，不改代码。
- 2026-10-24：L1 通过；合并 `l1-review` 到 `main`（暂不推送）。

## L2 performance engineering（2026-10-24 → 2026-10-25）

**AI 贡献**
- 编译期选项 `LOB_BASELINE`（基线 Session 的逐字节副本，D-045）；`scripts/l2_equivalence.sh`、`l2_rss.sh`、`l2_reproduce.sh`；
  等价性（16 个输入）、峰值 RSS 实测、一条命令复现（73 分钟）、massif 与机制探针验证；BENCHMARK §12、MODULE_NOTES/L2_EQUIVALENCE.md。
- AI 自查发现并修复的问题：
  1. 等价性脚本初版从 stdout 解析 `lob_replay` 的状态行，但它们写在 stderr：所有字段错位，`state_hash` 一列实为快照文件哈希、
     快照一列为空却仍判"一致"——提交前发现；改为读 stderr、要求每个哈希非空、失败即中止，并用 3 个变异构建做负对照；
  2. RSS 初版用 Python `getrusage(RUSAGE_CHILDREN)`：`ru_maxrss` 保留 exec 之前的峰值，带进约 10 MB 的 Python 自身占用
     （`/bin/true` 11.2 MB vs GNU time 1.1 MB）——改用 GNU time；
  3. 对已有构建目录换编译器重新配置时，CMake 删除缓存并静默丢掉 `-DLOB_BASELINE=ON`，"基线"目录变成优化构建——
     等价性脚本的构建检查拦下；复现脚本改为从空目录构建并断言缓存值；
  4. README 中复现耗时先写"约 70 分钟"，中途误改为"1.5–2 小时"，按实测（73 分钟）改回；
  5. 把 Windows PowerShell 下无法运行的构建命令直接给了作者（工具链只在 WSL 中）——README 补充说明与 `wsl` 包装形式；
  6. "只差一条命令"的 RSS 实验设计时已预见、结果也证实无法分离（命令向量在同一条命令上翻倍），如实记为未验证；
     20 万条的 RSS 差值仍未解释。
- 一次断电中断了 massif 前的测量；已落盘的数据保留并使用，未重复的部分在报告中注明。

**作者决策**
- 2026-10-24：L2 按步骤推进、逐步审查；选项名用 `LOB_BASELINE`；第 3 步 RSS 细节由 AI 决定。
- 2026-10-25：massif 验证限时 1 小时，可用内存约 5 GB。
