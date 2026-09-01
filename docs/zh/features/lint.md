# Lint

## 概述

clice 集成 clang-tidy 作为内置 lint 引擎。与独立的 clang-tidy 逐个处理每个 TU 不同，clice 的架构支持跨 TU 协调以消除重复工作。

**用法**：`clice lint [--workspace <dir>] [--workers <n>] [--index]`

在编译数据库中的每个翻译单元上运行 clang-tidy，使用
worker pool，打印诊断，并在发现问题时以非零状态退出。
`--index` 还会从相同的解析结果构建并持久化项目索引，
因此后续的 `clice index` 运行无需再做任何工作。

## 当前状态

- [x] 通过 CLI 进行项目级 lint（`clice lint`）
- [ ] 基本 clang-tidy 集成（单 TU、编辑器内诊断）
- [ ] 跨 TU 头文件去重
- [ ] 增量重新 lint（仅变更文件）
- [ ] Lint 结果缓存

## 跨 TU 优化

### 问题

clang-tidy 独立处理每个翻译单元。被 N 个源文件包含的头文件会被检查 N 次——这是乘法级开销，使大型代码库的项目级 lint 变得缓慢。

### clice 的方案

作为持久化服务器，clice 掌握完整的编译图，因此可以：

- [x] 追踪哪些头文件在多个 TU 间共享
- [ ] 对声明内容做哈希，跳过先前 TU 中已见的相同声明
- [ ] 带依赖感知地调度 lint 作业（共享头文件只 lint 一次，传播结果）
- [ ] 按内容哈希 + 检查配置缓存每个头文件的 lint 结果
- [ ] 单文件诊断去重（基础：移除单个 TU 内的重复项）
- [ ] 项目级诊断去重（高级：同一头文件跨 TU 的相同警告 → 只显示一次）

### 预期加速

对于有 H 个共享头文件和 N 个 TU 的项目，独立的 clang-tidy 的工作量是 O(N × H)。借助跨 TU 去重，clice 的设计面向增量检查——目标是每个头文件无论被多少个 TU 包含都只检查一次。

## clang-tidy 集成质量

影响 language server 中 clang-tidy 诊断质量的问题：

- [ ] 抑制系统头文件中宏产生的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] 对 preamble 中的预处理指令（头文件保护、宏）运行检查（[clangd#2501](https://github.com/clangd/clangd/issues/2501)、[clangd#160](https://github.com/clangd/clangd/issues/160)）
- [ ] 按检查类别配置诊断严重性（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 支持加载 clang-tidy 插件（[clangd#1458](https://github.com/clangd/clangd/issues/1458)）
- [ ] Clang 静态分析器支持（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 应用 clang-tidy 修复时清理替换内容（[clangd#429](https://github.com/clangd/clangd/issues/429)）
- [ ] 按版本控制 diff 过滤诊断（[clangd#822](https://github.com/clangd/clangd/issues/822)）
- [ ] NOLINT / NOLINTNEXTLINE / NOLINTBEGIN-END 注释抑制
- [ ] `.clangd` 配置中的 `Diagnostics.ClangTidy` 配置
- [ ] 面向 clang-tidy 性能的快速检查过滤
- [ ] 将 clang-tidy 的 fix-it 建议作为代码操作
- [ ] 诊断元数据：检查名称、文档 URL、来源标签

## 配置

检查选择目前使用内置的快速检查集，外加来自配置规则的每文件
编译标志。标准 `.clang-tidy` 配置文件的发现尚未接入。
