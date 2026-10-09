# 格式化

## 核心

- [x] 文档格式化（`textDocument/formatting`）
- [x] 范围格式化（`textDocument/rangeFormatting`）
- [x] 遵循 `.clang-format` 样式文件
- [x] 包含指令排序
- [x] 单次完成包含指令排序与重新格式化

## 样式解析

- [x] 从 `.clang-format` 自动检测样式——clang-format 从源文件所在目录向上逐级搜索至文件系统根目录
- [x] 当所有父目录中均未找到 `.clang-format` 时回退到 LLVM 默认样式

## 输入时与保存时钩子

- [ ] 输入时格式化（`textDocument/onTypeFormatting`）
- [ ] 保存时格式化集成

## 项目级格式化

除了 LSP `textDocument/formatting` 请求（用于格式化单个已打开文件），clice 还通过 CLI 提供项目级格式化。

- [ ] 使用 CLI `clice format` 进行批量格式化
- [ ] 并行格式化项目文件
- [ ] 增量格式化（仅格式化自上次运行以来修改的文件）
- [ ] 试运行/差异模式（显示将发生的更改）
