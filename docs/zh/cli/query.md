# 查询

## 概述

`clice query` 向持久化索引提出一个结构化的问题，并把答案以 JSON 形式打印到 stdout——供那些不想说 LSP、又想拿到语言服务器所掌握事实的脚本和 coding agent 使用。它直接从磁盘读取索引，正是 `clice index` 构建、编辑器会话持续更新的那一份，因此编辑器开着时它照常工作，编辑器没开时也不需要服务器。

**用法**：`clice query --method <question> [--workspace <dir>] [--configuration <tag>] [--fresh] [question options]`

## 支持的问题

- `symbolSearch --query <text> [--limit <n>] [--kind <Kind,...>]` 列出名称中包含该文本的符号，匹配度最高的排在最前，并附上各符号的种类、文件、行号和 id。
- `definition`、`readSymbol`、`references [--include-declaration]`、`callGraph [--direction callers|callees|both]` 和 `typeHierarchy [--direction supertypes|subtypes|both]` 回答关于单个符号的问题。符号由 `--name <name>`（可用 `--path` 进一步缩小范围）、`--symbol <id>`（此前的答案所带的 `#<hex>` id）或 `--path <file> --line <n>`（该行上定义的符号）指定。名称有歧义时，会列出候选的数量并要求改用 id。
- `documentSymbols --path <file>` 给出该文件的大纲。
- `compileCommand --path <file>` 给出编辑器编译该文件时会使用的命令，以及它的来源：文件自身的数据库条目、头文件的宿主源文件、规则的默认命令、根据邻近翻译单元推断出的命令，或内置的回退命令。
- `projectFiles [--filter all|source|header|module]` 列出构建涉及的文件；`fileDeps --path <file> [--direction includes|includers|both] [--depth <n>]` 和 `impactAnalysis --path <file>` 则沿包含关系图查询。

问题中的路径可以相对于工作区，也可以是绝对路径，答案中的路径一律是绝对路径；行号从 1 开始。

## 答案

每个答案都是一个 JSON 对象：成功时是 `{"result": ..., "stale": [...]}`；问题无法回答时（符号不存在、文件不存在、选项无效）是 `{"error": "...", "stale": [...]}`，退出码为 1。

`stale` 列出答案不得不略过其数据行的文件：它们在磁盘上的内容已经与建立索引时不一致，索引为它们保存的位置会指向移动过的文本。仅因所包含的头文件发生变化的文件，仍用它上一次的数据行作答。定义位于已过期文件中的符号会被报告为未找到，并给出文件名，这样读者就能区分“不存在”和“尚未建立索引”。索引从不读取编辑器中未保存的缓冲区：它描述的是磁盘上的内容。

`--fresh` 会在作答前把索引更新到与磁盘一致：若编辑器会话正打开着该工作区，由它的服务器执行这轮索引；否则由命令自己运行索引器，尚无索引时从零开始建立。两种情况下都只重新编译输入发生变化的翻译单元。不加 `--fresh` 时，没有索引的工作区只会返回错误。
