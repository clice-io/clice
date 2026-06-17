# 功能概览

clice 提供基于 LLVM/Clang 构建的 C++ 开发工具集。本节记录已实现的功能、计划中的功能以及相关的上游 issue 链接。

## LSP 编辑器功能

使用 clice 作为编辑器后端时可用的语言服务器协议功能。

| 功能        | 状态   | 页面                                      |
| ----------- | ------ | ----------------------------------------- |
| 代码补全    | 部分   | [completion](./completion.md)             |
| 悬停        | 已实现 | [hover](./hover.md)                       |
| 签名帮助    | 已实现 | [signature-help](./signature-help.md)     |
| 跳转到定义  | 部分   | [navigation](./navigation.md)             |
| 文档链接    | 部分   | [document-links](./document-links.md)     |
| 语义 Token  | 已实现 | [semantic-tokens](./semantic-tokens.md)   |
| Inlay Hints | 已实现 | [inlay-hints](./inlay-hints.md)           |
| 折叠范围    | 已实现 | [folding-ranges](./folding-ranges.md)     |
| 文档符号    | 已实现 | [document-symbols](./document-symbols.md) |
| 格式化      | 已实现 | [formatting](./formatting.md)             |
| 诊断        | 部分   | [diagnostics](./diagnostics.md)           |
| 代码操作    | 存根   | [code-action](./code-action.md)           |

## Lint

由 clang-tidy 驱动的项目级静态分析，具有 clice 独有的跨翻译单元优化。

| 功能            | 状态 | 页面              |
| --------------- | ---- | ----------------- |
| clang-tidy 集成 | 计划 | [lint](./lint.md) |

## 图例

- **已实现** — 核心功能正常工作，仅有细微差距
- **部分** — 关键子系统缺失（如模块支持）
- **存根** — 处理器存在但返回空/null
- **计划** — 已设计但尚未实现
