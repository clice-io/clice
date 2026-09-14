# Lint

## 概述

clice 将 clang-tidy 集成为内置 Lint 引擎。`clice lint` 借助常驻 worker 池，对整个编译数据库运行 `.clang-tidy` 中配置的检查，并输出一份合并后的报告。

**用法**：`clice lint [--workspace <dir>] [--configuration <tag>] [--workers <n>] [--index]`

对编译数据库中的每个翻译单元运行 clang-tidy，并使用 worker 池处理，
输出合并后的检查结果；发现问题时以非零状态退出。
`--index` 还会利用同一批解析结果构建并持久化项目索引，
因此后续运行 `clice index` 时无需再执行任何操作。

退出码：`0` 表示检查通过，`1` 表示发现了问题，`2` 表示本次运行未能按要求完成——某个翻译单元运行失败，或索引未能持久化。运行被中断时退出码为 `130`。

## 检查范围

- 编译数据库中列出、且位于工作区内的每个翻译单元。
- 被检查的单元所包含、且位于工作区内的每个头文件，并受 `.clang-tidy` 的头文件过滤项（`HeaderFilterRegex`、
  `ExcludeHeaderFilterRegex`、`SystemHeaders`）约束，这些过滤项的读取方式与 clang-tidy
  完全一致。
- 工作区之外的文件从不检查，无论构建是否把它们标记为系统头文件。`clice.toml` 中写有 `lint = false`
  的规则也会把工作区内匹配到的文件排除在外，例如随项目附带的第三方库：

```toml
[[rules]]
patterns = ["third_party/**"]
lint = false
```

每个翻译单元从最近的 `.clang-tidy` 读取配置，继承方式与 clang-tidy 一致。所有文件中的 `NOLINT`、`NOLINTNEXTLINE` 以及 `NOLINTBEGIN`/`NOLINTEND` 注释都会生效。头文件会在每个包含它的翻译单元中被检查，这与 clang-tidy 的行为一致；若多个翻译单元报出完全相同的结果，连 note 也一样，该结果在报告中只出现一次；报告按文件和位置排序，并保留 clang-tidy 附加的 note。

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
