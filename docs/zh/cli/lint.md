# Lint

## 概述

clice 将 clang-tidy 集成为内置 Lint 引擎。独立运行的 clang-tidy 会分别处理每个 TU，而 clice 的架构支持跨 TU 协调，从而消除重复工作。

**用法**：`clice lint [--workspace <dir>] [--workers <n>] [--index]`

对编译数据库中的每个翻译单元运行 clang-tidy，并使用 worker 池处理，
输出诊断；发现问题时以非零状态退出。
`--index` 还会利用同一批解析结果构建并持久化项目索引，
因此后续运行 `clice index` 时无需再执行任何操作。

## 当前状态

- [x] 通过 CLI 进行项目级 Lint（`clice lint`）
- [ ] 基础 clang-tidy 集成（单 TU、编辑器内诊断）
- [ ] 跨 TU 头文件去重
- [ ] 增量重新执行 Lint（仅检查已变更的文件）
- [ ] Lint 结果缓存

## 跨 TU 优化

### 问题

clang-tidy 会单独处理每个翻译单元。一个被 N 个源文件包含的头文件会被检查 N 次——这种成倍增加的开销会拖慢大型代码库的项目级 Lint。

### clice 的方案

作为掌握完整编译图的常驻服务器，clice 可以：

- [x] 跟踪哪些头文件在多个 TU 间共享
- [ ] 计算声明内容的哈希值，跳过对先前 TU 中已出现的相同声明的重复检查
- [ ] 根据依赖关系调度 Lint 作业（对共享头文件只执行一次 Lint，并传播结果）
- [ ] 以内容哈希值和检查配置为键，缓存各头文件的 Lint 结果
- [ ] 单文件诊断去重（基础：移除单个 TU 内的重复项）
- [ ] 项目级诊断去重（高级：同一头文件中跨 TU 出现的相同警告 → 只显示一次）

### 预期加速效果

对于有 H 个共享头文件和 N 个 TU 的项目，独立运行的 clang-tidy 的工作量为 O(N × H)。借助跨 TU 去重，clice 专为增量检查而设计——目标是无论被多少个 TU 包含，每个头文件都只检查一次。

## clang-tidy 集成质量

影响语言服务器中 clang-tidy 诊断质量的问题：

- [ ] 抑制系统头文件中宏产生的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] 对 Preamble 中的预处理指令（头文件保护、宏）执行检查（[clangd#2501](https://github.com/clangd/clangd/issues/2501)、[clangd#160](https://github.com/clangd/clangd/issues/160)）
- [ ] 可按检查类别配置诊断严重级别（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 支持加载 clang-tidy 插件（[clangd#1458](https://github.com/clangd/clangd/issues/1458)）
- [ ] 支持 Clang 静态分析器（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 应用 clang-tidy 修复时清理替换项（[clangd#429](https://github.com/clangd/clangd/issues/429)）
- [ ] 按版本控制差异过滤诊断（[clangd#822](https://github.com/clangd/clangd/issues/822)）
- [ ] 通过 NOLINT / NOLINTNEXTLINE / NOLINTBEGIN-END 注释抑制诊断
- [ ] `.clangd` 配置中的 `Diagnostics.ClangTidy` 配置项
- [ ] 用于提升 clang-tidy 性能的快速检查过滤
- [ ] 将 clang-tidy 的 fix-it 建议作为代码操作
- [ ] 诊断元数据：检查名称、文档 URL、来源标签

## 配置

检查项选择目前使用内置的快速检查集，以及配置规则中针对各文件的
编译标志。标准 `.clang-tidy` 配置文件的自动发现机制尚未接入。
