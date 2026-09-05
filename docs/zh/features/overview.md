# 语言服务器概览

clice 提供一套基于 LLVM/Clang 构建的 C++ 开发工具。本节介绍已实现和计划中的功能，并提供相关上游问题的链接。

## 功能

将 clice 用作编辑器后端时可用的语言服务器协议功能。

<!-- The status matrix is generated from the snapshot fixtures under
     tests/snap/. Do not edit the region between the GENERATED markers by
     hand — edit the fixtures (or OVERVIEW_ROWS in tools/docs/feature.ts)
     and run `node tools/docs/feature.ts update`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| 功能       | 状态                                       | 页面                               |
| ---------- | ------------------------------------------ | ---------------------------------- |
| 代码补全   | 支持 31 项                                 | [代码补全](./completion.md)        |
| 悬停       | 支持 34 项 · 部分支持 21 项 · 不支持 11 项 | [悬停](./hover.md)                 |
| 签名帮助   | 支持 14 项                                 | [签名帮助](./signature-help.md)    |
| 代码导航   | 支持 44 项 · 部分支持 14 项 · 不支持 34 项 | [代码导航](./navigation.md)        |
| 文档链接   | 支持 7 项 · 部分支持 1 项 · 不支持 1 项    | [文档链接](./document-links.md)    |
| 语义 Token | 支持 54 项 · 部分支持 4 项 · 不支持 10 项  | [语义 Token](./semantic-tokens.md) |
| 内联提示   | 支持 31 项 · 部分支持 6 项 · 不支持 4 项   | [内联提示](./inlay-hints.md)       |
| 折叠范围   | 支持 13 项 · 部分支持 2 项 · 不支持 6 项   | [折叠范围](./folding-ranges.md)    |
| 文档符号   | 支持 18 项 · 部分支持 2 项 · 不支持 7 项   | [文档符号](./document-symbols.md)  |
| 格式化     | 已实现                                     | [格式化](./formatting.md)          |
| 诊断       | 部分支持                                   | [诊断](./diagnostics.md)           |
| 代码操作   | 存根                                       | [代码操作](./code-action.md)       |

<!-- END GENERATED OVERVIEW -->

## 图例

有测试夹具（fixture）支撑的功能，按测试语料所确认的状态，统计文档中各项能力的数量：

- **支持** — 该能力可用；快照（snapshot）锁定其行为
- **部分支持** — 尚不完整；快照锁定目前可用的行为
- **不支持** — 已记录的功能缺口，已跟踪但尚未实现

尚未纳入测试夹具流水线的功能仍使用手动指定的标签：

- **已实现** — 核心功能可用，仅有少量不足
- **部分支持** — 关键子系统缺失（如模块支持）
- **存根** — 处理器已存在，但返回空结果或 null
- **计划中** — 已完成设计，但尚未实现
