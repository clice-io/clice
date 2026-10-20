# 编译命令

## 概述

`clice command` 显示 clice 编译某个文件时所用的命令，也能把编译命令转换成这种命令，供其他工具读取。它读取编译数据库，并向数据库中指定的编译器查询；既不需要索引，也不需要服务器。

**用法**：`clice command <file> [--workspace <dir>] [--configuration <tag>] [--json]`

**用法**：`clice command convert <compile_commands.json> [--workspace <dir>] [--configuration <tag>]`

**用法**：`clice command convert [--json] [--workspace <dir>] [--configuration <tag>] -- <compiler> <arguments>...`

## 文件的命令

`clice command <file>` 列出编译数据库为该文件记录的每个条目，按构建给出的优先级排序；排在第一的就是编辑器编译该文件时使用的条目，除非用户另选一条。每个条目会显示它所在的数据库、条目的原文（`command` 字符串按 clice 的方式拆分），以及 clice 依据该条目编译文件时所用的命令：先用 `clice.toml` 的[规则](../guide/configuration.md#rules)修改条目，再交给条目指定的编译器解析，由编译器给出目标平台和系统头文件目录，作为一条 `-cc1` 命令的参数。编译器无法查询时（没有安装，或拒绝了查询），错误信息会说明原因，此时显示的命令是 clice 自己对条目的解读。`--json` 则把同样的内容输出为一个 JSON 对象。

查找的数据库与编辑器为该文件加载的相同：即规则指定的数据库；若规则未指定，则是工作区根目录、其直接子目录，以及该文件与根目录之间各级目录中的 `compile_commands.json`。没有自身条目的文件，例如头文件（编辑器借用包含它的源文件的命令来编译它），会报告为没有条目。

```console
$ clice command src/main.cpp
/home/me/project/build/compile_commands.json:
  directory: /home/me/project/build
  command:   /usr/bin/g++ -std=c++20 -O2 -Werror -o main.o -c ../src/main.cpp
  resolved:  /usr/bin/g++ -cc1 -main-file-name main.cpp -triple x86_64-unknown-linux-gnu -O2 … -internal-isystem /usr/include/c++/13 … -std=c++20 … -x c++ /home/me/project/src/main.cpp
```

列出了文件的条目时退出码为 0；文件不存在或没有数据库列出它时为 1；参数无法解析时为 2。

## 转换命令

`clice command convert <compile_commands.json>`（也可以给出含有它的目录）输出这份数据库，并把其中每个条目的命令（`arguments` 或 `command`）替换为 clice 编译该条目文件时所用的 `arguments`（已按工作区 `clice.toml` 的规则修改）。条目保留 `file` 和 `output`；`directory` 写成绝对路径，相对路径以数据库所在目录为基准；其余字段不保留。clice 不编译的条目（例如汇编源文件）保留原来的命令。`clice command convert -- <compiler> <arguments>...` 转换单条命令：这条命令在当前目录下运行，且只编译一个 C 系源文件；结果输出为一行命令，加上 `--json` 时则输出为一个数据库条目。

转换后的命令是 clice 所基于的 Clang 的 `-cc1` 命令，适用于基于兼容版本 Clang 的工具。

所有命令都转换成功时退出码为 0；有编译器无法查询时为 1，此时仍会输出已转换的结果，无法解析的命令则按 clice 自己的解读输出；数据库无法读取，或命令没有编译任何 C 系源文件、编译了多个时，同样为 1；参数无法解析时为 2。
