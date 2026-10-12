# 语言服务器概览

clice 提供一套基于 LLVM/Clang 构建的 C++ 开发工具。本节介绍已实现和计划中的功能，并提供相关上游问题的链接。

## 功能

将 clice 用作编辑器后端时可用的语言服务器协议功能。

<!-- The status matrix counts the documented cases each feature page shows,
     in the order of the sidebar. Do not edit the region between the
     GENERATED markers by hand — run `pixi run update-feature-docs`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| 功能                                 | 状态                                      |
| ------------------------------------ | ----------------------------------------- |
| [代码补全](./completion.md)          | 支持 35 项 · 不支持 1 项                  |
| [悬停](./hover.md)                   | 支持 32 项 · 部分支持 9 项 · 不支持 20 项 |
| [签名帮助](./signature-help.md)      | 支持 12 项 · 部分支持 1 项 · 不支持 5 项  |
| [代码导航](./navigation.md)          | 支持 76 项 · 部分支持 8 项 · 不支持 27 项 |
| [重命名](./rename.md)                | 支持 4 项                                 |
| [诊断](./diagnostics.md)             | 支持 9 项                                 |
| [代码操作](./code-action.md)         | 支持 38 项                                |
| [语义 Token](./semantic-tokens.md)   | 支持 38 项 · 部分支持 3 项 · 不支持 7 项  |
| [内联提示](./inlay-hints.md)         | 支持 25 项 · 部分支持 1 项 · 不支持 4 项  |
| [文档链接](./document-links.md)      | 支持 7 项 · 部分支持 1 项                 |
| [文档符号](./document-symbols.md)    | 支持 15 项 · 部分支持 4 项 · 不支持 7 项  |
| [折叠范围](./folding-ranges.md)      | 支持 19 项                                |
| [选择范围](./selection-ranges.md)    | 支持 10 项                                |
| [格式化](./formatting.md)            | 支持 4 项                                 |
| [C++20 模块](./modules.md)           | 支持 7 项 · 不支持 1 项                   |
| [头文件上下文](./header-contexts.md) | 支持 4 项 · 不支持 1 项                   |
| [编译](./compilation.md)             | 支持 5 项 · 部分支持 1 项                 |

<!-- END GENERATED OVERVIEW -->

## 图例

每项功能均按状态统计其页面中记录的能力数量：

- **支持** — 该能力可用；快照（snapshot）锁定其行为
- **部分支持** — 尚不完整；快照锁定目前可用的行为
- **不支持** — 已记录的功能缺口，已跟踪但尚未实现
