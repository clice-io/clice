# 扫描

## 概述

`clice scan` 执行与服务器启动时相同的依赖扫描：从构建中的每个翻译单元出发，追踪各条包含指令并按编译器的搜索规则逐一解析，同时找出每个单元提供的模块。它只输出扫描结果，不编译任何文件，也不改动索引。

**用法**：`clice scan [--workspace <dir>] [--configuration <tag>] [--json | --runs <n>]`

构建范围由编译数据库列出：优先采用规则指定的数据库；若未指定，则与服务器启动时的查找方式相同，取项目根目录及其直接子目录中的 `compile_commands.json`（带有自身 `clice.toml` 或 `.clice/config.toml` 的子目录除外）；若一份都没有找到，报错信息会列出根目录下三层目录以内的数据库以及可加载它们的规则。摘要会统计扫描触达的文件数、其中的翻译单元数与已解析的包含指令数，并列出所有未找到对应头文件的包含指令。`--json` 则改为逐个输出扫描过的文件：是否为翻译单元、提供的模块、包含的头文件，以及未能解析的包含指令原文。

`--runs <n>` 用于测量扫描耗时：每轮先从头加载构建并扫描一遍（冷扫描，对应启动阶段），再复用首次扫描保留的结果重新扫描一遍（热扫描，对应重新加载编译数据库后），并输出两次耗时；最后一行给出两者的最小值与中位数。

扫描完成后退出码为 0；若没有可扫描的内容（项目根目录或配置不存在，或未找到编译数据库、数据库未列出任何文件）、参数同时请求两种输出或指定运行 0 轮，又或参数无法解析，则退出码为 2。

## 示例

### 包含指令

<!-- BEGIN GENERATED ITEMS: scan/includes -->

<!-- BEGIN CAPABILITY: supported -->

**解析包含指令**

每个文件列出其包含指令解析所得的头文件，以及查找不到的包含指令

扫描按编译器查找头文件的方式解析每条 `#include`：双引号括起的名称先从包含方所在目录查找，再按编译命令给出的搜索路径查找。扫描会继续追踪找到的头文件，并按所写的名称报告无法解析的包含指令。

```case
tests/scan/includes/01_resolution
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 模块

<!-- BEGIN GENERATED ITEMS: scan/modules -->

<!-- BEGIN CAPABILITY: supported -->

**查找模块提供方**

模块接口单元会连同它提供的模块一起列出

扫描会读取每个翻译单元的模块声明，输出中会指明每个模块由哪个文件构建。

```case
tests/scan/modules/01_providers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 多条编译命令

<!-- BEGIN GENERATED ITEMS: scan/commands -->

<!-- BEGIN CAPABILITY: supported -->

**同一文件对应多条命令**

编译数据库用多条命令编译同一文件时，该文件会汇集每条命令各自解析到的头文件

编译数据库为同一文件列出多条命令时，扫描会分别在每条命令的搜索路径下解析其包含指令，并在该文件下列出它们找到的所有头文件。

```case
tests/scan/commands/01_several_commands
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### 计时

<!-- BEGIN GENERATED ITEMS: scan/timing -->

<!-- BEGIN CAPABILITY: supported -->

**测量扫描耗时**

每轮先从全新加载开始冷扫描构建，再基于冷扫描保留的结果热扫描一遍

冷扫描对应服务器启动时的开销：读取并对每个文件做词法扫描。热扫描对应重新加载编译数据库时的开销：字节内容未变的文件直接复用此前的扫描结果，每个文件只需一次文件状态查询。最后一行给出冷热两次扫描耗时的最小值与中位数。

```case
tests/scan/timing/01_cold_and_warm
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
