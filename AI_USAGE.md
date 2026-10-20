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
