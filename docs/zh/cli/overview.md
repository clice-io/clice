# 命令行概览

clice 是单一可执行文件。编辑器启动 `clice serve` 并通过 LSP 与之通信；其余子命令在终端中驱动同一个引擎，共用其编译数据库处理、缓存和索引。

| 命令         | 作用                                                 | 页面                           |
| ------------ | ---------------------------------------------------- | ------------------------------ |
| `serve`      | 启动语言服务器。编辑器会代为启动，参见编辑器设置。   | [editors](../guide/editors.md) |
| `lint`       | 借助 worker 池对每个翻译单元运行 clang-tidy。        | [lint](./lint.md)              |
| `index`      | 提前为项目建立索引，让服务器热启动。                 | [index](./index.md)            |
| `format`     | 用 clang-format 格式化项目中的文件。                 | [format](./format.md)          |
| `inspect`    | 对单个源文件运行某项功能，将原始结果输出为 JSON。    |                                |
| `command`    | 显示文件编译时所用的命令，并为其他工具转换编译命令。 | [command](./command.md)        |
| `query`      | 向持久化索引查询符号、引用、调用图和文件。           | [query](./query.md)            |
| `refactor`   | 依据持久化索引在整个项目中重命名符号。               | [refactor](./refactor.md)      |
| `analyze`    | 报告重构所需的事实信息，例如模块依赖和循环依赖。     | [analyze](./analyze.md)        |
| `modularize` | 将第三方库封装为 C++20 模块，将程序改写为模块单元。  | [modularize](./modularize.md)  |
| `scan`       | 扫描构建的包含依赖与模块依赖，并为扫描计时。         | [scan](./scan.md)              |

`inspect` 没有单独页面：`clice inspect --help` 列出了它的各项操作和选项。

## 参数

每个子命令都以相同方式接受参数：

```
clice <command> [<operation>] [<target>...] [--option...]
```

- 承担多项工作的命令，用紧随其后的一个词指明具体操作：`clice query references`、`clice index stats`、`clice analyze modules edge`。有默认主任务的命令，后面不跟操作词时直接执行该任务：`clice index` 建立索引，`clice analyze modules` 输出概览，`clice command <file>` 显示该文件的编译命令。在任意命令或操作之后加上 `--help`，即可列出它接受的参数。
- 目标（即操作针对的符号或文件）跟在操作之后：`clice query references "shapes::Circle::area"`、`clice query document-symbols src/main.cpp`。凡是接受符号的位置，均以[定位符](./query.md#locators)（locator）指明符号；文件则以路径指明，相对于当前目录的相对路径或绝对路径均可；命令行上的其他路径（如分区文件或输出目录）也是如此。
- 选项放在操作之后。需要传入多个值时，重复给出该选项即可：`--scope 'src/**' --scope 'include/**'`。
- `--workspace <dir>` 指定项目根目录；未指定时，命令作用于当前目录所属的项目，查找方式与 git 查找仓库相同：从当前目录向上找最近的含有 `clice.toml`、`.clice/config.toml` 或 `compile_commands.json` 的目录（直接位于该目录下，或位于其 `build/` 子目录中），若均未找到则取当前目录本身（`inspect` 与编辑器一样，从目标文件所在位置向上查找）。若配置无法使用——配置文件无法解析、值类型有误，或存在命令无法遵照执行的选项——命令会以退出码 2 失败，并逐一标出问题及其在文件中的位置；`--configuration <tag>` 固定构建配置，优先级高于编辑器持久化的选择（参见[切换配置](../guide/configuration.md#switching-configurations)）；`-j`、`--jobs <n>` 指定 `index`、`lint` 和 `format` 同时处理的翻译单元或文件数量；环境变量 `CLICE_LOG_LEVEL` 控制输出到 stderr 的日志级别，默认为警告和错误（`serve` 的 stderr 会展示在编辑器中，因此默认从 `info` 起输出）；无论该环境变量设为多少，`serve`、`index` 和 `lint` 的会话日志始终保留 `info` 级别。

## 退出码

每个命令都以下列四种退出码之一结束，具体细节见各命令页面：

| 退出码 | 含义                                                                                                                 |
| ------ | -------------------------------------------------------------------------------------------------------------------- |
| `0`    | 命令完成了所请求的操作。                                                                                             |
| `1`    | 操作已完成，但结果为否定：存在 Lint 问题、有文件需要格式化、未找到符号或文件、重命名存在冲突，或有翻译单元索引失败。 |
| `2`    | 无法完成操作：参数或配置无法解析、没有可处理的对象（没有项目、没有编译数据库、没有翻译单元），或无法打开索引。       |
| `130`  | 运行被中断。                                                                                                         |
