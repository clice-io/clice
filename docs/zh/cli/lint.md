# Lint

## 概述

clice 将 clang-tidy 集成为内置 Lint 引擎。独立运行的 clang-tidy 会单独检查每个翻译单元，因此被多个源文件包含的头文件会随每个源文件各检查一遍。`clice lint` 对整个编译数据库运行同样的检查，每个声明只检查一次。

**用法**：`clice lint [--workspace <dir>] [--configuration <tag>] [--workers <n>] [--index] [--no-dedup] [--verify]`

用 worker 池对编译数据库中的每个翻译单元运行 clang-tidy，输出合并后的检查结果，发现问题时以非零状态退出。`--index` 还会利用同一批解析结果构建并持久化项目索引，因此后续运行 `clice index` 时无需再执行任何操作。

退出码：没有任何检查结果时为 `0`，存在检查结果时为 `1`，有翻译单元运行失败或下文的验证失败时为 `2`。

## 检查范围

- 编译数据库列出的、位于工作区内的每个翻译单元。
- 被检查的翻译单元所包含的、位于工作区内的每个头文件，受 `.clang-tidy` 中的头文件过滤选项（`HeaderFilterRegex`、`ExcludeHeaderFilterRegex`、`SystemHeaders`）约束，这些选项的读取方式与 clang-tidy 完全一致。
- 工作区之外的文件从不检查，无论构建是否将其标记为系统头文件。`clice.toml` 中带 `lint = false` 的规则还能把工作区内匹配到的文件也排除在外，例如随仓库一同提交的第三方库：

```toml
[[rules]]
patterns = ["third_party/**"]
lint = false
```

每个文件的配置来自最近的 `.clang-tidy`，并沿用 clang-tidy 的继承规则。`NOLINT`、`NOLINTNEXTLINE` 和 `NOLINTBEGIN`/`NOLINTEND` 注释在每个文件中都生效。每条检查结果只输出一次，按文件和位置排序，并附带 clang-tidy 为其附加的备注。

## 跨 TU 去重

clice 会为解析到的每个顶层声明计算内容哈希：声明自身的文本、它所依赖的编译状态（生效的宏、诊断 pragma、文件是否为系统头文件、编译标志），以及它引用的声明。两个翻译单元在相同标志下看到同一个头文件时，会为其中的声明算出相同的哈希，一个翻译单元已经检查过的声明在下一个翻译单元中会被跳过。实例化与其模板分开跟踪：翻译单元用新的实参实例化某个模板时，只会针对这些实例化重新检查该模板。

少数检查会同时查看多个声明（未使用的 using 声明、include 整洁性、文件范围内的命名冲突等）；这些检查会对每个翻译单元整体运行，其检查结果与其他结果一样合并。

- `--no-dedup` 像 clang-tidy 那样对每个翻译单元整体检查。
- `--verify` 两种方式都会运行，并在去重后的检查结果与整体运行的结果不一致时使本次运行失败。它会让每个翻译单元多解析一次，用途是在某个代码库上验证去重是否正确，不适合日常使用。

## clang-tidy 集成质量

影响语言服务器中 clang-tidy 诊断质量的问题：

- [ ] 抑制系统头文件中宏产生的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] 对 Preamble 中的预处理指令（头文件保护、宏）执行检查（[clangd#2501](https://github.com/clangd/clangd/issues/2501)、[clangd#160](https://github.com/clangd/clangd/issues/160)）
- [ ] 可按检查类别配置诊断严重级别（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 支持加载 clang-tidy 插件（[clangd#1458](https://github.com/clangd/clangd/issues/1458)）
- [ ] 支持 Clang 静态分析器（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 应用 clang-tidy 修复时清理替换项（[clangd#429](https://github.com/clangd/clangd/issues/429)）
- [ ] 按版本控制差异过滤诊断（[clangd#822](https://github.com/clangd/clangd/issues/822)）
- [x] 通过 NOLINT / NOLINTNEXTLINE / NOLINTBEGIN-END 注释抑制诊断
- [ ] `.clangd` 配置中的 `Diagnostics.ClangTidy` 配置项
- [ ] 用于提升 clang-tidy 性能的快速检查过滤
- [ ] 将 clang-tidy 的 fix-it 建议作为代码操作
- [ ] 诊断元数据：检查名称、文档 URL、来源标签
