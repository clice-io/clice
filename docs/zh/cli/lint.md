# Lint

## 概述

clice 将 clang-tidy 集成为内置 Lint 引擎。`clice lint` 借助常驻 worker 池，对项目的翻译单元运行 `.clang-tidy` 中的检查项，并输出一份合并后的报告。

**用法**：`clice lint [--workspace <dir>] [--configuration <tag>] [--jobs <n>] [--index] [--json]`

对编译数据库中属于下文检查范围的每个翻译单元运行 clang-tidy，输出合并后的检查结果，并在发现问题时以非零状态退出。`--index` 还会利用同一批解析结果构建并持久化项目索引，后续再运行 `clice index` 时便无事可做。`--json` 则改为以单个 JSON 对象输出报告：包含各项计数，以及每条检查结果的位置、检查项名称和附注（note）。

退出码：`0` 表示未发现问题，`1` 表示存在检查结果，`2` 表示本次运行未能按要求完成：参数无法解析、没有可检查的内容（没有项目、没有编译数据库或没有翻译单元）、某个翻译单元运行失败，或索引未能持久化。运行被中断时退出码为 `130`。

## 检查范围

- 编译数据库中列出且位于项目内的每个翻译单元。
- 受检翻译单元所包含且位于项目内的每个头文件，受 `.clang-tidy` 的头文件过滤项（`HeaderFilterRegex`、`ExcludeHeaderFilterRegex`、`SystemHeaders`）约束，读取方式与 clang-tidy 完全一致。
- 项目之外的文件从不检查，无论构建是否将其标为系统头文件。`clice.toml` 中设有 `lint = false` 的规则也会把项目内匹配的文件排除在外，例如随项目附带的第三方库：

```toml
[[rules]]
patterns = ["third_party/**"]
lint = false
```

每个翻译单元从最近的 `.clang-tidy` 读取配置，继承方式与 clang-tidy 一致。与 clang-tidy 相同，翻译单元编译命令启用的编译器警告会以其 `clang-diagnostic-<flag>` 名称计入检查结果，除非 `Checks` 将其关闭。所有文件中的 `NOLINT`、`NOLINTNEXTLINE` 以及 `NOLINTBEGIN`/`NOLINTEND` 注释均会生效。头文件会在每个包含它的翻译单元中受检，与 clang-tidy 的做法一致；多个翻译单元报出完全相同的结果（包括附注）时，报告中只出现一次；报告按文件和位置排序，并保留 clang-tidy 附加的附注。

编译数据库以多条命令列出的文件（例如构建为两个目标各编译一次），会在每条不同的命令下各检查一次。仅由后续命令报出的结果会标明该命令所属的条目，编号方式与 `clice command <file>` 列出该文件条目的顺序一致：`(entry 2 of main.cpp)`；在 JSON 报告中则记录为一个 `entry`，包含该翻译单元的 `file` 和 `index`。

## 示例

### 编译器警告

编译器警告会计入检查结果，除非检查项将其关闭。

<!-- BEGIN GENERATED ITEMS: lint/checks -->

<!-- BEGIN CAPABILITY: supported -->

**将编译器警告计入检查结果**

构建启用的警告会计入检查结果，除非检查项将其关闭

翻译单元的编译命令所启用的编译器警告会计入检查结果，并如 clang-tidy 一样以其 `clang-diagnostic-<flag>` 名称报告。如此处所示，`Checks` 以 `-*` 开头时，会关闭除其点名保留之外的所有警告。

```case
tests/lint/checks/02_compiler_warnings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 检查结果

共享头文件中的同一条检查结果只报告一次，`NOLINT` 注释可在所有文件中抑制检查结果。

<!-- BEGIN GENERATED ITEMS: lint/findings -->

<!-- BEGIN CAPABILITY: supported -->

**头文件检查结果只报告一次**

头文件在每个包含它的翻译单元中受检，其检查结果只报告一次

与 clang-tidy 一样，每个翻译单元都会检查其包含的头文件，因此共享头文件中的检查结果会由每个包含它的翻译单元分别产生。报告会将它们合并为一行，并按文件和位置列出每条检查结果。

```case
tests/lint/findings/01_header_findings_merge
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**抑制注释**

NOLINT 注释在头文件中与在受检文件中同样能抑制检查结果

`// NOLINT` 抑制所在行，`// NOLINTNEXTLINE` 抑制下一行，`// NOLINTBEGIN` 与 `// NOLINTEND` 抑制二者之间的所有内容；可作用于全部检查项，也可仅作用于括号内指明的检查项。翻译单元读取的每个文件中这些注释均会生效，读取方式与 clang-tidy 一致。

```case
tests/lint/findings/03_suppression_comments
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 排除文件

规则可将文件排除在报告之外。

<!-- BEGIN GENERATED ITEMS: lint/scope -->

<!-- BEGIN CAPABILITY: supported -->

**将文件排除在 Lint 之外**

设有 `lint = false` 的规则会将其匹配的文件排除在报告之外，头文件和翻译单元皆然

随项目附带的第三方库既不会作为独立的翻译单元受检，项目中的翻译单元包含它时也不会报告其中的问题。

```case
tests/lint/scope/01_keep_files_out
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
