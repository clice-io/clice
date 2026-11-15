# 格式化

## 格式化文档

格式化文件（`textDocument/formatting`）会按其 clang-format 风格重新排版，并在同一遍处理中为包含指令排序。

<!-- BEGIN GENERATED ITEMS: format/document -->

<!-- BEGIN CAPABILITY: supported -->

**格式化文档**

格式化文件会按其 `.clang-format` 指定的风格编排整个文件

```case
tests/format/document/01_format_document
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**包含指令排序**

格式化还会按风格要求为每组包含指令排序

```case
tests/format/document/04_include_sorting
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 格式化选区

范围格式化（`textDocument/rangeFormatting`）只重新格式化选区涉及的行。

<!-- BEGIN GENERATED ITEMS: format/range -->

<!-- BEGIN CAPABILITY: supported -->

**格式化选区**

格式化选区只重新格式化选区涉及的行

```case
tests/format/range/01_format_range
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 风格解析

每个文件都采用 clang-format 沿目录向上找到的最近一份 `.clang-format` 中的风格。

<!-- BEGIN GENERATED ITEMS: format/style -->

<!-- BEGIN CAPABILITY: supported -->

**最近的风格文件**

每个文件都采用上层目录中最近一份 `.clang-format` 的风格

```case
tests/format/style/01_nearest_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

上层目录中没有 `.clang-format` 的文件采用 LLVM 风格。

## 全项目格式化

除了格式化单个已打开文件的 LSP `textDocument/formatting` 请求外，clice 还在命令行提供全项目格式化：[`clice format`](../cli/format.md) 一次格式化构建中的多个文件，`--check` 则报告将会改动的内容。
