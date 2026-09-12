# logs/ — AI Coding 开发日志

## 日志来源声明（必读）

本队伍在全部开发过程中使用 **豆包（Doubao）AI 助手** 作为主要 AI Coding 工具，从 BSP 移植、驱动排障到上层应用开发全程在 AI 辅助下完成；另有部分 Claude Code 会话（官方日志采集器采集格式）。

**关于官方日志采集的说明**：大赛官方日志采集器（contest-log-collector）仅支持 Claude Code / OpenCode / Codex / AIoT-IDE 四种工具在工作区内的自动采集。豆包不在该列表内，对话无法被自动采集、也无法通过 `validate-log.py` 校验，因此本目录采用**人工整理的结构化开发日志**形式，如实记录每次关键开发动作、AI 参与方式与结论，作为 AI Coding 投入的辅助证明材料；Claude Code 会话以采集器原始 jsonl 格式留存。

## 目录内容

| 文件 | 说明 |
|---|---|
| `doubao-dev-log.md` | 完整开发过程日志（按阶段/日期），含每个功能模块的排障链路与 AI 参与点（阶段一~八，v44→v90） |
| `QQQuer/` | Claude Code 官方采集器导出（`manifest.json` + 会话 jsonl，schema v1.0） |
| `README.md` | 本说明 |

## 关键事实

- 开发仓库：`contest2026_471_sibakenailongshiyanshi`（分支 `dev-ai-contest-2026`，开发分支 `port/gd32h759-v1`）
- 硬件平台：慧勤智远 GD32H759IMT6 小系统板（Cortex-M7 @ 600MHz，LQFP176）
- 作品方向：新硬件平台适配（openvela/NuttX 移植 + 工业触控可视化 + 端侧 ADC 频谱 + PWM 亮度控制）
- 提交记录：`2c2e788`（9月8日，GUI 系统+驱动同步）、`8b8d93b`（9月11日，ADC+FFT 整理版）、后续 PR（9月12日，v90 完赛版合入）
- 固件版本：`firmware/` 下 v49~v90 关键交付版本（18 个），版本演进见根 README
