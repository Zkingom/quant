# M5 replay / snapshot / recovery —— 技术说明

状态：✅ 2026-10-21 验收（证据见 §9 与 [evidence/M5/](evidence/M5/)）。设计与特别审查问题的回答在 DESIGN §6（编码前写入，实现中的修正见 §6.7）。

## 1. 职责与边界

**负责**：命令与事件的稳定文本表示（v1）；带校验和的批次日志；快照导出、文件格式与恢复；以事件为真源的恢复；
命令身份与去重；提交协议（内存生效 / 持久化 / 确认 三态分离）；`lob_replay` 的回放、恢复、校验；崩溃一致性的测试与边界声明。

**不负责**：网络与多客户端（L3）；日志压缩；掉电持久性的验证；性能（M6）。

**依赖**：M1–M3 生产代码。M3 新增 `snapshot()/restore()`（唯一的生产接口变化，D-039）；M2 未改动。`lob::io` 不依赖 testkit。

## 2. 文件组织

```
include/lob/engine/snapshot.hpp      EngineSnapshot / SnapshotOrder / find_snapshot_violation
src/engine/snapshot.cpp              快照校验；matching_engine.cpp 中的 snapshot()/restore()
include/lob/io/checksum.hpp          FNV-1a 64
include/lob/io/text_format.hpp       v1 文本：配置、命令、事件、命令文件；ParseError
include/lob/io/journal.hpp           批次编码、scan_journal（尾部分类）、JournalCorruption
include/lob/io/sink.hpp              ByteSink / MemorySink / FileSink；read_file_if_exists；write_file_atomically
include/lob/io/snapshot_file.hpp     快照文件编码/解析；state_hash
include/lob/io/recovery.hpp          EventFold；recover()；RecoveryError
include/lob/io/session.hpp           Session（提交协议、去重、中毒）
include/lob/io/verify.hpp            verify_journal()
apps/lob_replay.cpp                  run / recover / verify
testkit/tools/gen_commands.cpp       lob_gen_commands（测试用命令文件生成器）
tests/io/*.cpp                       格式、日志、快照、恢复、会话、Golden、文件；test_io_faults.cpp（独立可执行文件，分配注入）
tests/golden/                        5 个手写场景 + 1 个固定种子 2000 条命令场景（.cmd / .journal / .snap）
tests/cli/golden_cli.sh, crash_restart.sh
scripts/evidence_m5.sh, scripts/mutation/m5_replay.py
```

## 3. 接口契约

| 接口 | 契约 |
|---|---|
| `MatchingEngine::snapshot()` | 规范顺序的完整状态；O(n) |
| `MatchingEngine::restore(s)` | `find_snapshot_violation(s)` 非空则抛 `invalid_argument`；之后的行为与原引擎逐事件相同 |
| `scan_journal(bytes)` | 返回完整批次、尾部状态（CLEAN/TORN）、有效长度；损坏抛 `JournalCorruption`（含字节偏移与行号） |
| `recover(journal, snapshot?, config?)` | 最后提交点的状态 + 水位 + 每个已提交命令的摘要与 seq 区间；边界/配置不一致抛 `RecoveryError` |
| `Session(recovered, sink, options)` | `sink` 必须从 `valid_bytes` 处追加；日志无头时写头 |
| `Session::submit(cmd)` | 返回 §4 的状态之一；已提交的 `cmd_seq` 绝不再执行；提交点之后不分配、不抛出 |
| `Session::snapshot_file()` | 最后提交点的状态；中毒时抛 `logic_error`（不暴露领先于日志的内存状态） |
| `verify_journal(scan)` | 逐批次重新执行并比较；资源失败批次以日志为准并重新同步 |
| `lob_replay` 退出码 | 0 成功、2 用法、3 输入格式、4 日志损坏、5 恢复被拒、6 会话中毒、7 命令未提交、8 verify 不一致、9 I/O |

## 4. 恢复状态机与提交协议

见 DESIGN §6.4（状态图）与 §6.2 Q2（每种情形下同一命令的语义）。会话 `submit` 的实现（[session.cpp:60](../src/io/session.cpp:60)）：

```
submit(cmd)
  中毒? → POISONED
  预备（可失败，但尚无任何效果）: digest = command_digest(cmd); committed_.reserve(+1)   失败 → NOT_APPLIED
  cmd_seq ≤ 水位 → 摘要相同 DUPLICATE（附原 seq 区间）/ 不同 CMD_SEQ_REUSED
  cmd_seq ≠ 水位+1 → CMD_SEQ_GAP
  (1) 执行，最多 1+R 次:
        bad_alloc 且 last_seq 未变 → 重试；用尽 → NOT_APPLIED
        bad_alloc 且 last_seq 已变 → RESIDUAL（已生效，事件以资源撤销结束）→ 继续
        length_error / overflow_error（last_seq 未变）→ REFUSED
        其他异常 → 中毒
  (2) 持久化（提交点）: encode_batch → sink.write（一次）→ [durable] sink.sync    失败 → 中毒
  (3) 记录（不分配）: 水位 = cmd_seq; committed_.push_back → COMMITTED[_WITH_RESOURCE_FAILURE]
```

**为什么第 (3) 步不能分配**：若批次已持久化而记录失败，内存水位不前进，重试会再次执行同一命令（DESIGN §6.7 发现 1）。
这一点由分配注入扫描与变异"提交记录在提交点后分配"共同固定。

## 5. 关键实现

### 5.1 批次与尾部分类（[journal.cpp:70](../src/io/journal.cpp:70)、[journal.cpp:149](../src/io/journal.cpp:149)）

`parse_batch` 返回三种结果之一：完整批次、`Incomplete`（在某个完整行都合法的前提下遇到不以 `\n` 结尾的剩余字节）、`Invalid`。
`scan_journal` 逐批解析：完整 → 推进有效长度；`Incomplete` → TORN，记录丢弃字节；`Invalid` → 抛出损坏（位置精确到字节与行）。
校验和覆盖从 `BATCH` 行开头到最后一个事件行的 `\n`。跨批次校验 `cmd_seq` 与事件 seq 都从 1 开始连续。

### 5.2 事件折叠（[recovery.cpp:30](../src/io/recovery.cpp:30)）

对 NEW 批次：首事件必须是与命令一致的 `ACCEPTED`（或单个 `REJECTED`）；随后每个 `TRADE` 必须指向对手侧在簿的 maker、价格等于 maker 价、
可成交、数量不超过双方剩余、两个剩余量一致；剩余为 0 时不得再有事件；否则恰好一个 `RESTED`（优先级 = `ACCEPTED` 的 seq）或
`CANCELLED(INSUFFICIENT_RESOURCES)`。CANCEL 批次：单个 `REJECTED` 或与在簿订单一致的 `CANCELLED(REQUESTED)`。
任何不一致都是损坏——这能发现校验和通过、但语义错误的批次（测试 `SemanticallyInconsistentBatchesAreCorruption`）。
折叠不检查价格-时间优先（那是审计器在测试中的职责）；它重建引擎**实际做过**的事。

### 5.3 快照恢复（[matching_engine.cpp:52](../src/engine/matching_engine.cpp:52)）

按 seq 升序逐个 `book_.add`：字段已校验、ID 唯一、容量满足、seq 严格递增、任何子集都不交叉——M2 `add` 的五个前置条件全部成立；
同价位内 FIFO 即 seq 顺序，因此队列被精确重建。最后设置 `last_seq_`，下一事件为 `last_seq + 1`。

### 5.4 `run` 的重启语义（[lob_replay.cpp:135](../apps/lob_replay.cpp:135)）

`run` 总是先恢复：读日志 → 可选快照 → `recover`（与命令文件配置比对）→ `FileSink(journal, valid_bytes)` 截断残缺尾部 → 会话。
命令文件从 cmd_seq 1 开始完整提交给会话，已提交的部分得到 `DUPLICATE` 被跳过——所以**崩溃后的重启就是用同一命令再运行一次**。

## 6. 一个完整的案例（`tests/golden/priority.*`）

命令 `NEW 5 5 BUY 103 10`，簿中卖 #1 102×3（seq 1）、#2 101×3（seq 3）、#3 101×3（seq 5）、#4 103×3（seq 7）：

1. 会话：摘要、预留；cmd_seq 5 = 水位 4 + 1；执行得到 `ACCEPTED 9`、`TRADE 10 … 2 BUY 101 3 7 0`、`TRADE 11 … 3 … 101 3 4 0`、
   `TRADE 12 … 1 … 102 3 1 0`、`TRADE 13 … 4 … 103 1 0 2`（价格优先，同价按 seq；成交价为 maker 价）。
2. 持久化：`BATCH 5 5 NEW 5 5 BUY 103 10` + 5 行事件 + `COMMIT 5 <校验和>`，一次写出。
3. 记录：水位 5，确认（`--echo` 时打印 `ACK 5 9 13`）。
4. 快照：`ORDER SELL 103 4 2 7`（剩 2，优先级 seq 7）。恢复时折叠：`TRADE 13` 把 #4 从 3 减为 2，与快照一致。

## 7. 对象生命周期与资源

- `Session` 持有引擎（值）与 `ByteSink&`（不拥有）；被丢弃后不再访问 sink。中毒的会话只能被丢弃。
- `FileSink` 拥有文件描述符（RAII 关闭）；`write_file_atomically` 在所有路径上关闭临时文件描述符。
- 资源清理在失败路径上由 ASan/LSan 预设下的全部故障测试覆盖（M4 已证明 LSan 在本环境有效）。

## 8. 边界条件与易错点

| 情形 | 行为 | 测试 |
|---|---|---|
| 日志为空 / 头部写到一半 | 视为空日志；会话写入新头 | `EmptyAndTornHeaderCountAsEmptyJournal`、崩溃测试 kill 1 |
| 每一个字节偏移处截断 | 恰好保留完整批次 | `TruncationAtEveryOffsetKeepsExactlyTheCompleteBatches` |
| NUL 填充的尾部 | 残缺 | `NulPaddedTailIsTorn` |
| 最后批次的任意单字节修改 | 损坏或整批丢弃，从不接受 | `NoSingleByteModificationOfTheLastBatchIsAccepted` |
| 中间批次损坏 / 缺失 | 损坏（不静默跳过） | `DamageBeforeTheTailIsCorruption` |
| 快照超前于日志 / 另一条历史 / 配置不同 | 拒绝恢复 | `InconsistentSnapshotAndJournalAreRefused` |
| 重复提交 / 内容不同 / 跳号 | DUPLICATE / CMD_SEQ_REUSED / CMD_SEQ_GAP，日志不变 | `IdentityChecksNeverExecuteACommittedCommandTwice` |
| 写到一半失败（任意偏移） | 中毒、无确认；恢复到最后提交点；续跑与不中断结果相同 | `WriteFailureAtAnyOffsetPoisonsAndRecoversToTheLastCommit` |
| `fsync` 失败 | 不确认，但批次可能已提交 → 重试得到 DUPLICATE | `SyncFailureIsNotAcknowledgedButMayBeCommitted` |
| seq 耗尽 | REFUSED，不中毒，之后仍可处理只需一个 seq 的命令 | `SequenceExhaustionIsRefusedWithoutEffect` |
| 非法 `Side` | 文本不可表达（语法错误） | `FieldSeparationIsStrict` |
| `\r` / 多余空格 / 前导零 / `-0` / 溢出 | 语法错误，报告行号 | `TextFormat.*`、`CommandFile.*` |

## 9. 验证证据

全部来自一次完整运行 `scripts/evidence_m5.sh`（[evidence/M5/](evidence/M5/)；基准提交 `c23977d`，运行期间只修改了 Markdown 文档，
`git diff c23977d -- . ':!*.md' ':!MODULE_NOTES'` 为空）。环境：WSL2 Ubuntu 24.04.1、i7-12700H（20 线程）、GCC 13.3.0 / Clang 18.1.3；
`/tmp` 为 ext4，源码树为 9P（[environment.txt](evidence/M5/environment.txt)）。

### 9.1 全量回归（[check_all_presets.log](evidence/M5/check_all_presets.log)）

删除 `build/` 后六个预设全部通过，每个 **195/195**（M1–M4 的 147 项 + M5 的 48 项：43 个 io 测试、3 个 io 分配注入测试、CLI Golden 与崩溃重启各 1 项），
`-Werror` 下零警告，墙钟 586 秒。崩溃测试与 Golden CLI 测试在每个预设（含 ASan/UBSan）下都运行过。

### 9.2 M5 验收测试（[io_tests.log](evidence/M5/io_tests.log)，gcc-release）

| 测试 | 实测 |
|---|---|
| 最后批次单字节修改（5 种替换 × 每个字节） | 417 次报告损坏、4 次整批丢弃、**0 次接受被修改的批次** |
| 快照恢复后的后续行为（10 画像 × 3 种长度 × 4 个切点） | 恢复的引擎在 **81,070** 条后续命令上与原引擎逐事件一致 |
| 分配注入全扫描（80 条命令，每条注入到不再命中为止） | **989** 个注入点；结果：重试后提交 3、资源失败提交 141、中毒 691、未生效 154，每种都按契约核对 |
| 长序列故障 + 重启（10 画像 × 2000 条命令，3% 注入） | 359 次"进程重启"、101 个资源失败批次、203 次未生效重试；每个画像最终恢复状态 = 会话状态，verify 无分歧，恢复后的引擎与会话后续逐事件一致 |
| M4 锚点 | `mixed` 画像仍逐字复现 M3 的计数（68558 / 54005 / …） |

### 9.3 进程崩溃（[crash_restart.log](evidence/M5/crash_restart.log)，gcc-release，持久模式）

30,000 条命令、每 250 条一次快照；**40 次 `SIGKILL` 全部落在运行中**，被杀时已确认的命令最多 15,225 条。每次被杀后恢复都成功，
恢复的水位从未低于已确认的最大 `cmd_seq`（常见"恢复 = 已确认 + 1"，即已提交但确认尚未输出，DESIGN Q2 的情形）；
最后让其跑完，日志与快照和不中断运行**逐字节相同**。40 次中没有一次落在单次 `write` 内部（尾部均为 CLEAN），残缺尾部由 §9.2 与日志的逐字节截断测试覆盖。

### 9.4 Golden（CLI 层，[golden_cli.log](evidence/M5/golden_cli.log)）

6 个场景全部 `ok`：`lob_replay run` 写出的日志与快照与仓库中的 Golden 文件逐字节相同，`verify` 通过，再次运行全部去重、日志不变。
同样的 Golden 在六个预设的 gtest 与 CLI 测试中都通过，即 GCC 与 Clang、Debug/Release/Sanitizer 下逐字节一致。

### 9.5 M4 长跑能力回归（[soak_summary.log](evidence/M5/soak_summary.log)）

完整长跑矩阵重跑：49 项目标检查全部达成（ASan 模式下 deep_book 的命令数低于目标门槛，未检查），0 项未达成；
10 个画像 GCC 与 Clang 的覆盖/状态/故障行逐字相同；墙钟 99 秒。

### 9.6 变异测试

| 表 | 结果 |
|---|---|
| M5（[mutations_m5_replay.log](evidence/M5/mutations_m5_replay.log)） | **15/15 杀死** |
| M3 回归 | 14/14 杀死 |
| M2 回归 | 10/11 杀死；存活者为 D-013 记录的等价变异 |

过程记录：同一套证据的上一次运行中，M5 表的"提交记录在提交点后分配"**存活**——分配注入扫描只注入前 8 次分配，到不了提交点之后的代码。
扫描改为覆盖每个分配点后，该变异被杀死（先在临时副本上单独确认，再完整重跑证据）。另有两次证据运行因脚本问题（相对路径、CRLF）失败，均为脚本缺陷，已修正后从头重跑。

### 9.7 未验证项

- **掉电 / 操作系统崩溃**：未验证；本机制在该场景下是受限恢复机制（DESIGN §6.6）。
- **`kill -9` 落在单次 `write` 内部**：40 次中 0 次；残缺尾部只由单元测试覆盖。
- **引擎内部的 `logic_error`（R3）**：正常代码不可达；中毒路径由写失败共用的机制测试。**`length_error`（R10）**：不可实际触发，仅代码审查。
- **libc++**：未测试。
- **9P 文件系统上的 `fsync` 语义**：未知；崩溃测试在 ext4 的 `/tmp` 上进行。

## 10. 修改任务（供独立练习；只给需求与验收，不给实现）

**T5-1 日志压缩。** 在快照之后丢弃已被快照覆盖的批次，生成一个从 `cmd_seq = k+1` 开始的新日志。
验收：新日志格式需要记录起点（版本升级为 v2 并保留对 v1 的读取）；恢复要求"快照 + 压缩日志"与"完整日志"得到相同状态；
去重在被压缩掉的范围内仍能拒绝重复提交（说明你用什么代替被丢弃的摘要）；崩溃测试在压缩过程中被杀死也能恢复。

**T5-2 去重时返回原结果。** `DUPLICATE` 目前只返回原批次的 seq 区间。让 `lob_replay run --echo` 在遇到重复时从日志读取并重新输出原事件（幂等应答）。
验收：重启后重复的命令输出与第一次运行逐字节相同的事件；不增加会话的内存占用超过每命令常数。

**T5-3 批量提交（group commit）。** 在持久模式下把连续 N 条命令的批次合并为一次 `write + fsync`，确认推迟到合并写入之后。
验收：日志格式不变；任何崩溃点恢复后不丢失已确认命令（扩展崩溃模型测试）；在 M6 中测量 fsync 次数与吞吐的变化（无数据前不得下结论）。

**T5-4 二进制格式。** 为日志设计一个带长度前缀与 CRC 的二进制 v2 编码，与文本 v1 互相转换。
验收：`text → binary → text` 逐字节恒等；尾部分类规则对二进制格式同样成立（逐字节截断测试与单字节修改测试移植通过）。

**T5-5 掉电模拟。** 设计一个比"保留已写字节前缀"更悲观的崩溃模型（例如未 `fsync` 的页面可以以任意子集、任意顺序丢失），说明本实现在该模型下的保证。
验收：书面模型 + 测试；若保证不成立，在 DESIGN §6.6 中如实记录，并给出修复方案（例如每批次 `fsync` 之外还需要什么）。

## 11. 总结

- **已验证能力**：见 §9。
- **遗留问题与边界**：掉电持久性未验证；崩溃测试中 `kill -9` 未落在单次 `write` 内部（残缺尾部由单元测试覆盖）；
  `logic_error` 来自引擎的中毒路径只能通过共享的中毒机制间接测试（引擎正常代码中不可达）；`length_error`（R10）不可实际触发，仅代码审查；
  libc++ 未测试；日志不可压缩。
- **对 M6 的交付**：`lob_gen_commands` 生成的命令文件与 `lob_replay run` 可直接作为端到端基准的输入与被测对象；
  基准必须区分非持久模式与持久模式（后者受 `fsync` 支配）。
