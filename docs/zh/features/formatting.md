# 格式化

## 核心

- [x] 文档格式化（`textDocument/formatting`）
- [x] 范围格式化（`textDocument/rangeFormatting`）
- [x] 遵守 `.clang-format` 样式文件
- [x] Include 排序
- [x] Include 排序 + 重新格式化单次完成

## 样式解析

- [x] 从项目 `.clang-format` 自动检测样式
- [x] 回退到 LLVM 默认样式
- [ ] 输入时格式化（`textDocument/onTypeFormatting`）
- [ ] 保存时格式化集成

## 项目级格式化

除了 LSP `textDocument/formatting` 请求（格式化单个打开的文件），clice 还通过 CLI 提供项目级格式化。

- [ ] CLI `clice format` 批量格式化
- [ ] 跨项目文件并行格式化
- [ ] 增量格式化（仅自上次运行以来修改的文件）
- [ ] 试运行/差异模式（显示将要更改的内容）

## 变更记录

| 日期 | 变更                                 | PR  |
| ---- | ------------------------------------ | --- |
| —    | 文档格式化、范围格式化、include 排序 | —   |
