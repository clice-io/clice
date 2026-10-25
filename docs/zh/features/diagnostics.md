# 诊断

## 核心

- [x] Clang 诊断（错误、警告、备注）
- [x] 严重程度映射（错误、警告）
- [x] 带源位置的诊断范围
- [x] Preamble 中的预处理指令所产生的警告（`#warning`、`#pragma message`、宏重定义）
- [x] 跨文件诊断的文件 URI 转换
- [x] 拉取式诊断模型（`textDocument/diagnostic`），用于声明支持它的客户端；其他客户端的诊断仍由服务端推送
- [x] 每个缺失的 `#include` 都会报告，而不只是第一个，其后的代码也照常检查
- [x] clang 在解析之前就拒绝的编译命令（例如未知的目标平台），会在文件顶部显示 clang 给出的原因
- [ ] 显示源自头文件的诊断的完整包含链（[clangd#1392](https://github.com/clangd/clangd/issues/1392)）

  ```
  // current:  "In included file: expected ';'"
  // expected: "In main.cpp → utils.h → detail/impl.h: expected ';'"
  ```

- [x] Preamble 头文件中的模板实例化错误诊断，在后台索引编译过该文件后显示（[clangd#137](https://github.com/clangd/clangd/issues/137)）

## 所包含的头文件中的错误

头文件中的错误会在引入该头文件的 `#include` 处报告，因此会显示在你正在编辑的文件中。

<!-- BEGIN GENERATED ITEMS: diagnostics/headers -->

<!-- BEGIN CAPABILITY: supported -->

**所包含的头文件中的错误**

所包含的头文件中的错误会显示在包含该头文件的那一行

每个被包含的头文件的第一个错误会报告在对应的 `#include` 行，并在头文件中的出错位置附上一条备注。头文件中的其他错误归属于头文件本身，会在打开该头文件时显示。

```case
tests/diagnostics/headers/01_errors_on_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**头文件函数体中的错误**

头文件某个函数体中的错误，会在后台索引按保存后的内容编译过该文件后显示

与 clangd 一样，编辑器编译文件时会跳过文件顶部所包含的头文件中的函数体：文件打开以及编辑后的重建因此快上数倍。后台索引会编译这些函数体，它在其中发现的错误——内联函数的错误、模板以文件给出的实参实例化时的错误，或这类模板调用的、文件自身的泛型 Lambda 中的错误——只要文件中仍是它编译过的文本，就会显示在该文件上：编辑会让它们消失，直到文件保存并再次编译。在 `[project]` 中设置 `skip_header_bodies = false`，编辑器也会解析这些函数体。

```case
tests/diagnostics/headers/09_skipped_bodies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板实例化

模板内部的错误或警告会报告在实例化该模板的代码处。

<!-- BEGIN GENERATED ITEMS: diagnostics/templates -->

<!-- BEGIN CAPABILITY: supported -->

**模板实例化中的错误**

模板内部的错误会显示在代码触发实例化的位置

如果模板使用给定实参实例化时出错，错误会报告在触发实例化的代码处，而非模板内部。错误附带的备注会沿实例化链指向模板中的出错位置。文件顶部所包含的头文件中的模板由后台索引实例化，而不是由编辑器的编译实例化，因为后者会跳过该头文件中的函数体：这类模板的错误要等索引按保存后的内容编译过该文件后才会显示。

```case
tests/diagnostics/templates/01_instantiation_errors
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 备注

clang 为错误附加的备注会作为相关信息随错误一同提供；不接收相关信息的客户端会在诊断消息中看到这些备注。

<!-- BEGIN GENERATED ITEMS: diagnostics/notes -->

<!-- BEGIN CAPABILITY: supported -->

**作为相关信息的备注**

clang 为错误附加的备注会指向其中提到的位置

错误会附带解释该错误的备注，例如某个名称首次定义的位置，或某次调用无法使用的候选项。每条备注都是该错误的相关信息，因此编辑器会将其显示在错误旁，并支持跳转到对应位置。

```case
tests/diagnostics/notes/01_related_information
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 没有编译数据库的文件

即使编译数据库中未列出某个文件，clice 仍会使用推测的命令编译它。

<!-- BEGIN GENERATED ITEMS: diagnostics/guidance -->

<!-- BEGIN CAPABILITY: supported -->

**没有编译数据库**

未在任何编译数据库中列出的文件会使用推测的命令编译，并给出相应提示

即使 `compile_commands.json` 中没有对应条目，clice 仍会使用推测的命令编译文件。如果因此找不到某个被包含的文件，文件顶部会显示一条警告，说明原因并提供配置指南的链接。一旦编译数据库中列出了该文件，警告就会消失，剩下的就是代码本身的错误。

```case
tests/diagnostics/guidance/01_no_database
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 标签

- [x] `-Wdeprecated` 诊断和涉及已弃用头文件与别名的 clang-tidy 检查结果的 `Deprecated` 标签
- [x] 未使用变量/参数警告和 `misc-unused-*` clang-tidy 检查结果的 `Unnecessary` 标签

## 发布

- [x] 编译完成时推送诊断
- [x] 文件关闭时清除诊断
- [x] 在 VS Code 中，“问题”面板只在有编辑器或标签页显示某个文件时列出该文件的诊断，即使文件关闭后另一个扩展仍让它保持打开也是如此
- [x] 保存头文件或修改 `.clang-tidy` 后，clice 会让客户端重新请求包含它的已打开文件，这些文件随之重新编译——拉取诊断的客户端收到诊断刷新请求，其他客户端收到语义高亮刷新请求，前提是客户端支持该请求；与 clangd 一样，诊断以头文件保存后的内容为准，而不是它未保存的缓冲区
- [x] 按文件对诊断分组（打开的文件和头文件）
- [x] 诊断 `code` 字段：警告对应的选项（`-Wunused-variable`）、其他诊断在 clang 中的名称、clang-tidy 检查名
- [x] `codeDescription` 将 clang-tidy 检查结果链接到该检查的文档
- [ ] `codeDescription` 中提供指向 Clang 文档的链接
- [x] 诊断 `source` 字段区分 clang 与 clang-tidy
- [ ] 可配置计算诊断前的防抖延迟（[clangd#1471](https://github.com/clangd/clangd/issues/1471)）
- [ ] 后台索引完成后重新计算已打开文件的诊断（[clangd#2604](https://github.com/clangd/clangd/issues/2604)）

## 诊断抑制

clang 误报的诊断可以在配置中按诊断代码隐藏，注释则能屏蔽所在行的诊断。

<!-- BEGIN GENERATED ITEMS: diagnostics/suppression -->

<!-- BEGIN CAPABILITY: supported -->

**抑制诊断**

`suppress` 按诊断代码隐藏诊断

clang 在某些代码上误报的诊断，可以按它显示的诊断代码隐藏：编译器警告的选项、其他诊断在 clang 中的名称，或 clang-tidy 检查名。文件的其他功能照常工作，`clice lint` 也仍会报告这条诊断。

```case
tests/diagnostics/suppression/01_by_code
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 使用 `// NOLINT` 注释抑制诊断
- [x] 使用 `// NOLINTNEXTLINE` 注释抑制诊断
- [x] `// NOLINTBEGIN` / `// NOLINTEND` 块级抑制
- [ ] include-cleaner 诊断支持 `NOLINT`（[clangd#1982](https://github.com/clangd/clangd/issues/1982)）
- [ ] 配置文件中可按诊断类别配置严重程度（[clangd#1937](https://github.com/clangd/clangd/issues/1937)）
- [ ] 根据版本控制 diff 过滤诊断——仅显示变更行附近的警告（[clangd#822](https://github.com/clangd/clangd/issues/822)）

## 诊断操作

- [x] 编译器和 clang-tidy 附在诊断上的修复，作为快速修复提供
- [x] clang-tidy 的修复会像 `clang-tidy --fix` 那样做清理：删掉内容后不会留下悬空的 `,` 或 `:`
- [x] 修复向模块单元添加的 include 放进其全局模块片段，片段只开一次

## 头文件诊断

- [ ] 针对未使用或缺少的 `#include` 指令提供 include-cleaner 诊断
- [x] 抑制头文件中的 static inline 函数所触发的 `-Wunused-function` 误报（[clangd#1211](https://github.com/clangd/clangd/issues/1211)）

  ```cpp
  // utils.h
  static inline int helper() { return 42; }
  // should NOT warn "unused function" when checking header standalone
  ```

- [x] 声明只在头文件的函数体中用到时，不会误报 `-Wunused-function` 或 `-Wunused-member-function`——编辑器会跳过这些函数体；确实未使用的声明由后台索引报告（[clangd#363](https://github.com/clangd/clangd/issues/363)、[clangd#636](https://github.com/clangd/clangd/issues/636)）
- [ ] 传递来自头文件的 `-Wpadded` 等布局警告（[clangd#1429](https://github.com/clangd/clangd/issues/1429)）
- [ ] 抑制 Preamble 优化导致的 `-Wempty-translation-unit` 误报（[clangd#2358](https://github.com/clangd/clangd/issues/2358)）
- [ ] 跨头文件边界的线程安全分析（[clangd#2386](https://github.com/clangd/clangd/issues/2386)）

## clang-tidy 集成

clang-tidy 对已打开的文件执行项目 `.clang-tidy` 中启用的检查，检查结果与编译器的诊断一同显示。

<!-- BEGIN GENERATED ITEMS: diagnostics/clang_tidy -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy 检查结果**

clang-tidy 对已打开的文件执行最近的 `.clang-tidy` 中启用的检查

每条检查结果都会标明对应的检查，并提供指向该检查文档的链接。检查配置来自从文件所在目录向上查找到的最近的 `.clang-tidy`，与 `clang-tidy` 自身读取配置的方式一致。

```case
tests/diagnostics/clang_tidy/01_findings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有配置文件时**

没有 `.clang-tidy` 时，会运行少量检查

项目即使没有 `.clang-tidy`，仍会运行几项不易误报的检查，例如未使用的声明检查，编辑器会将这些声明淡化显示。

```case
tests/diagnostics/clang_tidy/02_default_checks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**使用 NOLINT 抑制诊断**

`NOLINT` 注释会抑制所在行的检查结果，也会抑制编译器警告

`// NOLINT` 抑制所在行的检查结果，`// NOLINTNEXTLINE` 则抑制下一行的检查结果；两者都可针对所有检查或括号中指定的检查。clang-tidy 在文件上运行时，这些注释也会抑制相应行的编译器警告。

```case
tests/diagnostics/clang_tidy/04_nolint
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**修复以快速修复的形式提供**

检查结果附带的修复会以快速修复的形式提供

检查随结果附带的修复会出现在该结果所在位置的快速修复选项中；应用修复后，该检查结果就会消失。

```case
tests/diagnostics/clang_tidy/05_quick_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关闭 clang-tidy**

设置 `clang_tidy = false` 后，clang-tidy 不会在已打开的文件上运行

关闭 clang-tidy 后，无论 `.clang-tidy` 中启用了哪些检查，已打开的文件都只显示编译器的诊断。

```case
tests/diagnostics/clang_tidy/09_turned_off
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] 对编辑器来说过慢的检查，以及在编辑中的代码上不可靠的检查，不在编辑器中运行；`clice lint` 会运行它们
- [x] `performance-use-std-move` 既不在编辑器中运行，也不在 `clice lint` 中运行：在 clice 所基于的 LLVM 中，它处理从 PCH 或模块中读取的类时会崩溃
- [x] 对 `.clang-tidy` 的修改会在已打开文件下次编译时生效
- [x] 某个 clang-tidy 检查崩溃时，只暂停该文件的 clang-tidy，不影响它的其他功能
- [x] 抑制源自系统头文件中宏的 clang-tidy 警告（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）
- [ ] Clang 静态分析器支持（[clangd#905](https://github.com/clangd/clangd/issues/905)）
- [ ] 特定版本的 clang-tidy 文档链接（[clangd#2136](https://github.com/clangd/clangd/issues/2136)）
- [ ] 对位于代码之前的预处理指令进行诊断（[clangd#2501](https://github.com/clangd/clangd/issues/2501)）

## 诊断显示

- [ ] 限定类型名的正确诊断范围——为完整名称标注下划线，而不只是基名（[clangd#1035](https://github.com/clangd/clangd/issues/1035)）

  ```cpp
  ns::Inner obj(42);
  //  ^^^^^ only base name underlined
  // should underline "ns::Inner"
  ```

- [ ] 将优化备注（`-Rpass`）显示为诊断（[clangd#2519](https://github.com/clangd/clangd/issues/2519)）
- [x] 列出整个项目的警告：`clice lint`（[clangd#1973](https://github.com/clangd/clangd/issues/1973)）

## 配置文件诊断

- [x] 格式错误的 `.clang-tidy` 会在它所配置的文件顶部报告，并且不运行任何检查（[clangd#2313](https://github.com/clangd/clangd/issues/2313)）
- [ ] 为格式错误的 `.clang-format` 文件报告诊断（[clangd#2591](https://github.com/clangd/clangd/issues/2591)）

## 诊断正确性

应正确处理的已知问题：

- [ ] 宏重定义警告指向同一位置（[clangd#2479](https://github.com/clangd/clangd/issues/2479)）
- [ ] 针对嵌套数组初始化给出错误的 `-Wmissing-braces` 修复建议（[clangd#2434](https://github.com/clangd/clangd/issues/2434)）
- [ ] Preamble 失效导致诊断消息混杂时出现无效的严重级别 0（[clangd#2124](https://github.com/clangd/clangd/issues/2124)）
- [ ] 未声明标识符诊断被与更正相关的诊断隐藏（[clangd#547](https://github.com/clangd/clangd/issues/547)）
- [ ] `--include` 文件缺失时产生误导性的后续诊断（[clangd#2229](https://github.com/clangd/clangd/issues/2229)）
