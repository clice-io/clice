# 命令行概览

clice 只有一个二进制。编辑器启动 `clice serve` 并通过 LSP 与它通信；其余子命令在终端里驱动同一个引擎，共用它的编译数据库处理、缓存和索引。

| 命令         | 作用                                                     | 页面                           |
| ------------ | -------------------------------------------------------- | ------------------------------ |
| `serve`      | 启动语言服务器。编辑器会替你启动，见编辑器设置。         | [editors](../guide/editors.md) |
| `lint`       | 用 worker 池对每个翻译单元运行 clang-tidy。              | [lint](./lint.md)              |
| `index`      | 提前为工作区建立索引，让服务器热启动。                   | [index](./index.md)            |
| `format`     | 用 clang-format 格式化工作区中的文件。                   | [format](./format.md)          |
| `inspect`    | 对一个源文件运行某一项功能，把原始结果打印成 JSON。      |                                |
| `command`    | 显示一个文件编译时所用的命令，并为其他工具转换编译命令。 | [command](./command.md)        |
| `query`      | 向持久化索引查询符号、引用、调用图和文件。               | [query](./query.md)            |
| `refactor`   | 依据持久化索引在整个工作区重命名一个符号。               | [refactor](./refactor.md)      |
| `analyze`    | 报告重构所需的事实，例如模块依赖和循环依赖。             | [analyze](./analyze.md)        |
| `modularize` | 把第三方库封装成 C++20 模块，把程序改写成模块单元。      | [modularize](./modularize.md)  |
| `scan`       | 扫描构建的 include 和模块依赖，并为扫描计时。            | [scan](./scan.md)              |

`inspect` 现在已可使用但还没有页面，它的操作和选项通过 `clice inspect --help` 查看。

## 参数

每个子命令都以同样的方式接受参数：

```
clice <command> [<operation>] [<target>...] [--option...]
```

- 能做几件事的命令，用紧跟其后的一个词指明要做哪一件：`clice query references`、`clice index stats`、`clice analyze modules edge`。有一项首要职能的命令，后面不跟词时就执行这项职能：`clice index` 建立索引，`clice analyze modules` 给出概览，`clice command <file>` 显示该文件的编译命令。在任何命令或操作后面加上 `--help`，都会列出它接受的参数。
- 目标，即操作所针对的符号或文件，跟在操作之后：`clice query references "shapes::Circle::area"`、`clice query document-symbols src/main.cpp`。凡是接受符号的地方，都用[定位串](./query.md#locators)（locator）指明符号；文件则用路径指明，可以相对于工作区（对 `inspect` 而言相对于当前目录），也可以是绝对路径。命令自己读写的文件，例如分区文件或输出目录，相对于当前目录。
- 选项放在操作之后。一个选项要取多个值时，就重复给出它：`--scope 'src/**' --scope 'include/**'`。
- `--workspace <dir>` 指定工作区，默认为当前目录（`inspect` 与编辑器一样，取文件所在的项目）；`--configuration <tag>` 固定构建配置，其优先级高于编辑器持久化的选择（参见[切换配置](../guide/configuration.md#switching-configurations)）；`-j`、`--jobs <n>` 指定 `index`、`lint` 和 `format` 同时处理多少个单元或文件；`--log-level` 设置向 stderr 输出哪些日志，默认为警告和错误（编辑器会显示 `serve` 的 stderr，它从 `info` 级别起输出）；无论该选项取什么值，`serve`、`index` 和 `lint` 的会话日志都保留 `info` 级别。
