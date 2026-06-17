# Lint

## 概述

clice 集成 clang-tidy 作为内置代码检查引擎。与独立的 clang-tidy 逐个处理每个翻译单元不同，clice 的架构支持跨翻译单元协调以消除重复工作。

**用法**：`clice lint`（尚未实现）

## 当前状态

- [ ] 基本 clang-tidy 集成（单翻译单元、编辑器内诊断）
- [ ] 通过 CLI 进行项目级 lint（`clice lint`）
- [ ] 跨翻译单元头文件去重
- [ ] 增量重新 lint（仅变更文件）
- [ ] Lint 结果缓存

## 跨翻译单元优化

### 问题

clang-tidy 独立处理每个翻译单元。被 N 个源文件包含的头文件会被检查 N 次 — 这是乘法级开销，使得大型代码库的项目级 lint 非常缓慢。

### clice 的方案

作为持久化服务器，clice 掌握完整的编译图，因此可以：

- [x] 追踪哪些头文件在多个翻译单元间共享
- [ ] 对声明内容哈希以跳过先前翻译单元中已检查的相同声明
- [ ] 带依赖感知的 lint 作业调度（共享头文件只 lint 一次，传播结果）
- [ ] 按内容哈希 + 检查配置键缓存头文件 lint 结果
- [ ] 报告去重诊断（同一头文件中的相同警告只显示一次）

### 预期加速

对于具有 H 个共享头文件和 N 个翻译单元的项目，独立的 clang-tidy 的工作量为 O(N × H)。通过跨翻译单元去重，clice 将其降低到 O(N + H) — 每个头文件无论被多少翻译单元包含都只检查一次。

## clang-tidy 集成质量

影响语言服务器中 clang-tidy 诊断质量的问题：

- [ ] 抑制来自系统头文件宏的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] 在预编译头中的预处理指令（头文件保护、宏）上运行检查（[clangd#2501](https://github.com/clangd/clangd/issues/2501)、[clangd#160](https://github.com/clangd/clangd/issues/160)）
- [ ] 按检查类别配置诊断严重性（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 支持加载 clang-tidy 插件（[clangd#1458](https://github.com/clangd/clangd/issues/1458)）
- [ ] Clang 静态分析器支持（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 应用 clang-tidy 修复时清理替换内容（[clangd#429](https://github.com/clangd/clangd/issues/429)）
- [ ] 按版本控制 diff 过滤诊断（[clangd#822](https://github.com/clangd/clangd/issues/822)）

## 配置

遵守项目树中的标准 `.clang-tidy` 配置文件。

## 变更记录

| 日期 | 变更 | PR |
| ---- | ---- | -- |
| —    | 存根实现、依赖图追踪 | — |
