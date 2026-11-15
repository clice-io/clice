# 编译命令

## 概述

`clice command` 显示 clice 编译文件时使用的命令，也可将编译命令转换为这种形式供其他工具读取。它读取编译数据库并向其中指定的编译器查询，既不需要索引，也不需要服务器。

**用法**：`clice command <file> [--workspace <dir>] [--configuration <tag>] [--json]`

**用法**：`clice command convert <compile_commands.json> [--workspace <dir>] [--configuration <tag>]`

**用法**：`clice command convert [--json] [--workspace <dir>] [--configuration <tag>] -- <compiler> <arguments>...`

## 文件的编译命令

`clice command <file>` 按构建给出的优先级顺序列出各编译数据库中该文件的所有条目；用户另选之前，编辑器使用排在第一位的条目编译该文件。每个条目会显示所属数据库、条目原文（`command` 字符串按 clice 的拆分方式展开），以及 clice 按该条目编译文件时实际使用的命令：先按[规则](../guide/configuration.md#rules)（定义于 `clice.toml`）编辑条目，再交给条目指定的编译器解析，将目标平台与系统头文件目录补全为一条 `-cc1` 命令的参数。若其中某些目录含有 clice 编译时不可见的文件——被 clang 自身内置头文件替代的其他编译器内置头文件（GCC 私有目录中的头文件，或另一个 clang 的内置头文件），以及列表中位置更靠后、原编译器也永远不会读到的同名文件——每个这类目录都会附一行 `hidden` 统计其数量（参见[查询编译器](../design/command-resolve.md#asking-the-compiler)）。若因编译器未安装或拒绝查询而无法查询，错误信息会说明原因，显示的命令则改为 clang 自身驱动代替该编译器读取的驱动命令，与规则设有 `query_toolchain = false` 时一致；基于兼容版本 Clang 构建的工具读取该命令的结果与 clice 相同。`--json` 将相同内容输出为单个 JSON 对象。

所读取的编译数据库与编辑器为该文件加载的一致：优先使用规则指定的数据库；未指定时，则是项目根目录、其直接子目录，以及从该文件到根目录之间各级目录中的 `compile_commands.json`；带有自身 `clice.toml` 或 `.clice/config.toml` 的目录及其下所有内容属于另一个项目。本身没有条目的文件（例如头文件，编辑器会借用包含它的源文件的命令来编译）会报告为没有条目。

```console
$ clice command src/main.cpp
/home/me/project/build/compile_commands.json:
  directory: /home/me/project/build
  command:   /usr/bin/g++ -std=c++20 -O2 -Werror -o main.o -c ../src/main.cpp
  resolved:  /usr/bin/g++ -cc1 -main-file-name main.cpp -triple x86_64-unknown-linux-gnu -O2 … -internal-isystem /usr/include/c++/13 … -std=c++20 … -x c++ /home/me/project/src/main.cpp
  hidden:    112 of the files in /usr/lib/gcc/x86_64-linux-gnu/13/include

settings:  background_index = false (/home/me/project/clice.toml:6)
           the rest by default
```

条目之后，`settings` 列出各条[规则](../guide/configuration.md#rules)为该文件决定的设置——`background_index`、`skip_function_body`、`readonly`、`lint` 等按文件生效的字段——每项都会标明决定它的规则及其所在文件和起始行号；没有任何规则设置的字段取默认值。`--json` 会在 `settings` 下列出所有字段，给出各自的 `value`，若由某条规则决定则还会给出该 `rule`。

列出文件条目时退出码为 0；文件不存在或没有任何数据库列出该文件时为 1；读取期间数据库发生变化、参数无法解析，或项目及其配置无法加载时为 2。

## 转换命令

`clice command convert <compile_commands.json>`（或传入包含该文件的目录）会输出整份数据库，并将每个条目的命令（`arguments` 或 `command`）替换为 clice 编译该条目文件时使用的 `arguments`（已应用项目 `clice.toml` 中的规则编辑）。条目保留 `file` 和 `output`；`directory` 转为绝对路径（原为相对路径时以数据库所在目录为基准）；其余字段一概略去。clice 不编译的条目（如汇编源文件）保留原命令。输出的命令会略去其他编译器的内置头文件目录，因为命令行无法表达其中被隐藏的文件（参见[已知限制](../design/command-resolve.md#known-limitations)）。`clice command convert -- <compiler> <arguments>...` 转换单条命令——该命令在当前目录下运行，且只编译一个 C 族源文件——默认输出为一行命令，加上 `--json` 时输出为单个数据库条目。

转换后的命令是构建 clice 所用 Clang 的 `-cc1` 命令，适用于基于兼容版本 Clang 构建的工具。编译器无法查询的命令则保持为驱动命令，即 clang 自身驱动代替该编译器读取的那条命令。

所有命令均转换成功时退出码为 0；有编译器无法查询时为 1，此时仍会先输出转换结果，其中无法查询的命令输出为上述驱动命令；数据库无法读取、单条命令未编译任何 C 族源文件或编译了多个源文件，或者参数无法解析时为 2。
