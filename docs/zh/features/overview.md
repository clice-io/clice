# 功能概览

clice 提供一套基于 LLVM/Clang 构建的 C++ 开发工具。本节记录已实现的功能、计划中的功能，以及相关上游 issue 链接。

## LSP 编辑器功能

使用 clice 作为编辑器后端时可用的语言服务器协议功能。

<!-- The status matrix is generated from the snapshot fixtures under
     tests/snap/. Do not edit the region between the GENERATED markers by
     hand — edit the fixtures (or OVERVIEW_ROWS in tools/docs/feature.ts)
     and run `node tools/docs/feature.ts update`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| 功能        | 状态                                | 页面                                      |
| ----------- | ----------------------------------- | ----------------------------------------- |
| 代码补全    | 30 项支持                           | [completion](./completion.md)             |
| 悬停        | 34 项支持 · 21 项部分 · 11 项不支持 | [hover](./hover.md)                       |
| 签名帮助    | 14 项支持                           | [signature-help](./signature-help.md)     |
| 代码导航    | 44 项支持 · 14 项部分 · 34 项不支持 | [navigation](./navigation.md)             |
| 文档链接    | 7 项支持 · 1 项部分 · 1 项不支持    | [document-links](./document-links.md)     |
| 语义 Token  | 54 项支持 · 4 项部分 · 10 项不支持  | [semantic-tokens](./semantic-tokens.md)   |
| Inlay Hints | 31 项支持 · 6 项部分 · 4 项不支持   | [inlay-hints](./inlay-hints.md)           |
| 折叠范围    | 13 项支持 · 2 项部分 · 6 项不支持   | [folding-ranges](./folding-ranges.md)     |
| 文档符号    | 18 项支持 · 2 项部分 · 7 项不支持   | [document-symbols](./document-symbols.md) |
| 格式化      | 已实现                              | [formatting](./formatting.md)             |
| 诊断        | 部分                                | [diagnostics](./diagnostics.md)           |
| 代码操作    | 存根                                | [code-action](./code-action.md)           |

<!-- END GENERATED OVERVIEW -->

## Lint

由 clang-tidy 驱动的项目级静态分析，具有 clice 独有的跨 TU 优化。

| 功能            | 状态 | 页面              |
| --------------- | ---- | ----------------- |
| clang-tidy 集成 | 部分 | [lint](./lint.md) |

项目级 CLI（`clice lint`）已实现；编辑器内实时
诊断和跨 TU 去重仍在计划中。

## 图例

fixture 支持的功能统计了测试语料库在各状态下固定的已记录能力：

- **supported** — 功能可用；snapshot 固定了行为
- **partial** — 不完全；snapshot 固定了当前可用的功能
- **unsupported** — 已记录的缺口，已跟踪但尚未实现

尚未进入 fixture 管线的功能保留手动分配的标签：

- **Implemented** — 核心功能可用，仅有细微差距
- **Partial** — 关键子系统缺失（如模块支持）
- **Stub** — 处理器存在但返回空/null
- **Planned** — 已设计但尚未实现
