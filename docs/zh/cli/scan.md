# 扫描

## 概述

`clice scan` 运行服务器启动时执行的依赖扫描：从构建中的每个翻译单元出发，追踪并按编译器的搜索方式解析各条包含指令，同时找出每个单元提供的模块。它只输出扫描的结果，既不编译任何文件，也不改动索引。

**用法**：`clice scan [--workspace <dir>] [--configuration <tag>] [--json | --runs <n>]`

构建由它的编译数据库列出：即规则指定的数据库，若未指定，则是工作区下的每一份 `compile_commands.json`。摘要统计扫描到的文件数、其中的翻译单元数和已解析的包含指令数，并列出所有未能找到对应头文件的包含指令。`--json` 则改为输出每个扫描过的文件：它是否为翻译单元、它提供的模块、它包含的头文件，以及它无法解析的包含指令（保留源码中的写法）。

`--runs <n>` 为扫描计时：每轮先从头加载构建并扫描（冷扫描，与启动时相同），再基于首次扫描保留的内容重新扫描（热扫描，与重新加载数据库之后相同），并输出两次扫描的耗时；最后一行分别给出冷扫描和热扫描耗时的最小值和中位数。

扫描完成时退出码为 0；没有可扫描的内容（工作区或配置不存在，或找不到编译数据库、数据库未列出任何文件），或参数同时要求两种输出、要求零次运行时为 1；参数无法解析时为 2。

## 示例

### 头文件包含

<!-- BEGIN GENERATED ITEMS: scan/includes -->

<!-- BEGIN CAPABILITY: supported -->

**解析包含指令**

每个文件列出其包含指令解析到的头文件，以及未能找到对应头文件的包含指令

扫描按编译器查找头文件的方式解析每一条 `#include`：对于双引号括起的名称，先在写有该指令的文件所在目录中查找，再按命令给出的搜索路径查找。扫描会继续追踪找到的头文件，并按名称报告无法解析的包含指令。

```case
tests/scan/includes/01_resolution
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 模块

<!-- BEGIN GENERATED ITEMS: scan/modules -->

<!-- BEGIN CAPABILITY: supported -->

**查找模块的提供方**

模块接口单元会与它提供的模块一同列出

扫描会读取每个翻译单元的模块声明，因此输出中写明了每个模块由哪个文件构建。

```case
tests/scan/modules/01_providers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 多条命令

<!-- BEGIN GENERATED ITEMS: scan/commands -->

<!-- BEGIN CAPABILITY: supported -->

**一个文件，多条命令**

数据库用多条命令编译同一个文件时，该文件包含每条命令各自解析到的头文件

数据库为同一个文件列出多条命令时，扫描会分别按每条命令的搜索路径解析该文件的包含指令，并在该文件的条目中列出由此找到的所有头文件。

```case
tests/scan/commands/01_several_commands
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 计时

<!-- BEGIN GENERATED ITEMS: scan/timing -->

<!-- BEGIN CAPABILITY: supported -->

**为扫描计时**

每一轮先从全新加载开始冷扫描一遍构建，再在冷扫描保留的结果上热扫描一遍

冷扫描是服务器启动时付出的代价：读取并词法扫描每一个文件。热扫描是编译数据库重新加载时付出的代价：内容没有变化的文件，直接复用它们的扫描结果，每个文件只花一次状态查询。最后一行给出两者的最小值和中位数。

```case
tests/scan/timing/01_cold_and_warm
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
