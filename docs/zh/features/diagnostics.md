# 诊断

## 核心

诊断包含已打开文件上 clang 报告的错误与警告、clang-tidy 的检查结果，以及 clice 自身关于项目配置的提示消息。

- 支持拉取诊断（`textDocument/diagnostic`）的客户端按需主动请求；其余客户端则在每次编译完成时由服务端推送。
- 每个缺失的 `#include` 都会报告，而不只是第一个，其后的代码仍照常检查。
- Preamble 中的预处理指令产生的警告（`#warning`、`#pragma message`、宏重定义）与其他警告一样正常显示。
- clang 在解析之前就拒绝的编译命令（例如未知的目标平台），会在文件顶部显示 clang 给出的原因。
- 头文件内部的模板实例化错误，会在后台索引编译过该文件后显示（[clangd#137](https://github.com/clangd/clangd/issues/137)）。

## 所包含头文件中的错误

头文件内部的错误会报告在引入它的 `#include` 处，从而直接显示在你正在编辑的文件中。

<!-- BEGIN GENERATED ITEMS: diagnostics/headers -->

<!-- BEGIN CAPABILITY: supported -->

**所包含头文件中的错误**

所包含头文件内部的错误显示在包含它的那一行

每个被包含头文件的首个错误会报告在对应的 `#include` 行上，并在头文件内的出错位置附上一条备注。该头文件的其余错误归属头文件自身，打开该头文件时才会显示。

```case
tests/diagnostics/headers/01_errors_on_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**头文件函数体中的错误**

头文件函数体内部的错误会在后台索引按已保存内容编译该文件后显示

与 clangd 一样，编辑器编译文件时会跳过文件顶部所包含头文件中的函数体，使文件打开与编辑后重新编译快上数倍。后台索引会编译这些函数体，它在其中发现的问题——内联函数的错误、模板以该文件给出的实参实例化时的错误，或这类模板所调用的该文件自身泛型 Lambda 中的错误——在文件内容仍与索引编译时一致的期间会显示在该文件上；一旦编辑文件，这些诊断就会撤下，待文件保存并重新编译后再恢复。为某个头文件设置 `skip_function_body = false` 的规则会让编辑器也解析其函数体；为第三方代码设置 `skip_function_body = true` 的规则则会让后台索引同样跳过这些函数体，其中出现的问题不再显示。

```case
tests/diagnostics/headers/09_skipped_bodies
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模板实例化

模板内部的错误或警告会报告在触发实例化的代码处。

<!-- BEGIN GENERATED ITEMS: diagnostics/templates -->

<!-- BEGIN CAPABILITY: supported -->

**模板实例化中的错误**

模板内部的错误显示在代码请求实例化的位置

模板以给定实参实例化出错时，错误会报告在请求实例化的代码处，而非模板内部。附带的备注会沿着实例化链一路指向模板内出错的具体位置。文件顶部所包含头文件中的模板由后台索引实例化，编辑器自身的编译则会跳过这些头文件的函数体，因此这类错误会在后台索引按已保存内容编译该文件后显示。

```case
tests/diagnostics/templates/01_instantiation_errors
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 备注

clang 附在错误上的备注会作为相关信息随错误一并传递；不支持相关信息的客户端则会在诊断消息正文中看到这些备注。

<!-- BEGIN GENERATED ITEMS: diagnostics/notes -->

<!-- BEGIN CAPABILITY: supported -->

**作为相关信息的备注**

clang 附在错误上的备注指向其中提及的源码位置

错误会附带解释原因的备注，例如某个名称首次定义的位置，或某次调用无法选用的候选函数。每条备注都作为该错误的相关信息提供，编辑器会将其显示在错误旁边，并支持跳转到对应位置。

```case
tests/diagnostics/notes/01_related_information
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 没有编译数据库的文件

编译数据库未列出的文件仍可使用 clice 推测的编译命令参与编译。

<!-- BEGIN GENERATED ITEMS: diagnostics/guidance -->

<!-- BEGIN CAPABILITY: supported -->

**没有编译数据库**

未在任何编译数据库中列出的文件使用推测的命令编译，并给出相应提示

即使没有对应的 `compile_commands.json` 条目，文件仍会使用 clice 推测的命令编译。项目没有任何编译数据库时，编辑器会显示消息提示；若推测的命令导致某个头文件无法解析，文件顶部会显示警告说明原因，并附上配置指南链接。一旦编译数据库列出了该文件，警告便会消失，只留下代码本身的错误。

```case
tests/diagnostics/guidance/01_no_database
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 标签

- `Deprecated` 标记 `-Wdeprecated` 诊断以及涉及已弃用头文件和别名的 clang-tidy 检查结果。
- `Unnecessary` 标记未使用的变量和参数警告以及 `misc-unused-*` 检查结果。

## 发布

- 只接收推送诊断的客户端会在文件编译完成时收到诊断，保存编辑过的文件时还会重新收到一份；关闭文件即清除诊断。
- 保存头文件或修改 `.clang-tidy` 会触发受影响的已打开文件重新编译：拉取诊断的客户端会收到重新拉取的请求，其余客户端则会收到新推送的诊断。与 clangd 一样，诊断以头文件已保存的内容为准，而非未保存的缓冲区。
- 在 VS Code 中，“问题”面板仅在有编辑器或标签页展示某个文件时列出其诊断，即使关闭标签页后仍有其他扩展保持该文件处于打开状态也是如此。
- 每条诊断都带有 `code`——编译器警告对应的选项（如 `-Wunused-variable`）、其他诊断在 clang 中的内部名称、clang-tidy 检查项名称，或 clice 自有的诊断名称（如 `module-not-built`、`inferred-compile-command`）——以及标明来源的 `source`：clang、clang-tidy 或 clice。clang-tidy 检查结果会链接到对应检查项的文档，worker 崩溃则会链接到 clice 的问题跟踪器。

## 诊断抑制

clang 误报的诊断可在配置中按诊断代码隐藏，行内注释则可屏蔽所在行的诊断。

<!-- BEGIN GENERATED ITEMS: diagnostics/suppression -->

<!-- BEGIN CAPABILITY: supported -->

**抑制诊断**

`suppress` 按诊断代码隐藏诊断

clang 在某些代码上误报的诊断可按其显示的诊断代码隐藏：编译器警告选项、其他诊断在 clang 中的名称，或 clang-tidy 检查项名称。文件的其余功能照常工作，`clice lint` 也仍会报告该诊断。

```case
tests/diagnostics/suppression/01_by_code
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

`// NOLINT`、`// NOLINTNEXTLINE` 以及 `// NOLINTBEGIN` / `// NOLINTEND` 注释会屏蔽对应行上的 clang-tidy 检查结果；当 clang-tidy 在该文件上运行时，还会一并屏蔽这些行上的编译器警告。

## 诊断操作

- 编译器与 clang-tidy 附在诊断上的修复建议会作为快速修复提供。
- clang-tidy 的修复会按 `clang-tidy --fix` 的方式做清理：移除代码后不会留下悬空的 `,` 或 `:`。
- 修复向模块单元新增的 include 会放入其全局模块片段，且该片段只开启一次。

## 头文件诊断

- 头文件中定义但当前文件未调用的 `static inline` 函数不会触发 `-Wunused-function` 警告（[clangd#1211](https://github.com/clangd/clangd/issues/1211)）。
- 仅在头文件函数体内部用到（而编辑器跳过了这些函数体）的声明，不会误报 `-Wunused-function` 或 `-Wunused-member-function`：真正未使用的声明由后台索引报告（[clangd#363](https://github.com/clangd/clangd/issues/363)、[clangd#636](https://github.com/clangd/clangd/issues/636)）。

## clang-tidy 集成

clang-tidy 会按项目 `.clang-tidy` 启用的检查项在已打开文件上运行，检查结果与编译器诊断一同显示。

<!-- BEGIN GENERATED ITEMS: diagnostics/clang_tidy -->

<!-- BEGIN CAPABILITY: supported -->

**clang-tidy 检查结果**

clang-tidy 按最近的 `.clang-tidy` 启用的检查项在已打开文件上运行

每条检查结果都会标明对应的检查项，并链接到该检查项的文档。检查配置取自文件上层目录中最近的 `.clang-tidy`，读取方式与 `clang-tidy` 自身一致。

```case
tests/diagnostics/clang_tidy/01_findings
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**没有配置文件时**

没有 `.clang-tidy` 时仍会运行少量默认检查

没有 `.clang-tidy` 的项目仍会运行几项极少误报的检查，例如未使用的声明，编辑器会将其淡化显示。

```case
tests/diagnostics/clang_tidy/02_default_checks
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**略过的检查项**

对编辑器而言过慢的检查项会点名告知，不会悄然略过

已打开的文件既不运行对编辑器而言过慢的检查项，也不运行在编辑中的代码上不可靠的检查项。对于 `.clang-tidy` 启用的慢速检查项，clice 会提示一次其名称及运行方法；`clice lint` 则会将两类检查项一并运行。

```case
tests/diagnostics/clang_tidy/03_editor_exclusions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**使用 NOLINT 屏蔽**

`NOLINT` 注释屏蔽所在行的检查结果，同时也会屏蔽编译器警告

`// NOLINT` 屏蔽自身所在行，`// NOLINTNEXTLINE` 屏蔽下一行，可针对全部检查项或仅针对括号中列出的检查项。当 clang-tidy 在文件上运行时，它们也会一并屏蔽对应行上的编译器警告。

```case
tests/diagnostics/clang_tidy/04_nolint
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**修复建议转为快速修复**

附带修复建议的检查结果会提供快速修复操作

检查项附在检查结果上的修复会出现在该位置的快速修复列表中；应用修复后该检查结果即会清除。

```case
tests/diagnostics/clang_tidy/05_quick_fix
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**关闭 clang-tidy**

`clang_tidy = false` 让已打开文件不再运行 clang-tidy

关闭 clang-tidy 后，无论 `.clang-tidy` 启用了哪些检查项，已打开的文件都只显示编译器自身的诊断。

```case
tests/diagnostics/clang_tidy/09_turned_off
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**运行慢速检查项**

`slow_clang_tidy_checks = true` 也会运行对编辑器而言过慢的检查项

对编辑器而言过慢的检查项（如 `misc-const-correctness`）只有在显式启用时才会在已打开文件上运行，届时该文件的每次编译都要等待它们完成。

```case
tests/diagnostics/clang_tidy/20_slow_checks
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- 对编辑器而言过慢的检查项仅在 `[diagnostics] slow_clang_tidy_checks` 显式开启时才在已打开文件上运行，且除非显式设为 `false`，否则 clice 会列出被略过的检查项；在编辑中的代码上不可靠的检查项始终略过；`clice lint` 则会运行全部检查项。
- `performance-use-std-move` 在编辑器与 `clice lint` 中均被排除：在 clice 所基于的 LLVM 版本中，该检查处理从 PCH 或模块读取的类时会崩溃。
- 对 `.clang-tidy` 的修改会在已打开文件下次编译时生效。
- 某个 clang-tidy 检查项崩溃时，只会暂停该文件上的 clang-tidy，不影响文件的其他功能。
- 源自系统头文件中宏的 clang-tidy 警告会被过滤（[clangd#1587](https://github.com/clangd/clangd/issues/1587)、[clangd#2000](https://github.com/clangd/clangd/issues/2000)）。
- 格式有误的 `.clang-tidy` 会在受其配置的文件顶部报错，且不运行任何检查（[clangd#2313](https://github.com/clangd/clangd/issues/2313)）。
- `clice lint` 列出整个项目的警告（[clangd#1973](https://github.com/clangd/clangd/issues/1973)）。
