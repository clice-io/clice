# 格式化

## 格式化文档

格式化文件（`textDocument/formatting`）会按该文件的 clang-format 样式调整排版，并在同一次操作中对 include 指令排序。

<!-- BEGIN GENERATED ITEMS: format/document -->

<!-- BEGIN CAPABILITY: supported -->

**格式化文档**

格式化文件会按照其 `.clang-format` 指定的样式调整整个文件的排版

```case
tests/format/document/01_format_document
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**include 指令排序**

格式化还会按样式要求对每组 include 指令排序

```case
tests/format/document/04_include_sorting
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 格式化选区

范围格式化（`textDocument/rangeFormatting`）只会重新格式化选区涉及的行。

<!-- BEGIN GENERATED ITEMS: format/range -->

<!-- BEGIN CAPABILITY: supported -->

**格式化选区**

格式化选区只会重新格式化选区涉及的行

```case
tests/format/range/01_format_range
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 样式解析

每个文件都使用 clang-format 沿目录层级向上查找到的最近的 `.clang-format` 文件中的样式。

<!-- BEGIN GENERATED ITEMS: format/style -->

<!-- BEGIN CAPABILITY: supported -->

**最近的样式文件**

每个文件都使用沿目录层级向上查找到的最近的 `.clang-format` 文件中的样式

```case
tests/format/style/01_nearest_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 当所有父目录中均未找到 `.clang-format` 时回退到 LLVM 默认样式

## 输入时与保存时钩子

- [ ] 输入时格式化（`textDocument/onTypeFormatting`）
- [ ] 保存时格式化集成

## 项目级格式化

除了用于格式化单个已打开文件的 LSP `textDocument/formatting` 请求，clice 还通过 CLI 提供项目级格式化：[`clice format`](../cli/format.md) 会格式化参与构建的所有文件，每次同时处理多个文件，使用 `--check` 则会报告将会修改的内容。

- [ ] 增量格式化（仅格式化自上次运行以来修改的文件）
