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
