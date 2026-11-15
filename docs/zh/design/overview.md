# 源码概览

clice 是一个 C++ 语言服务器，针对早期服务器不推倒重来便无法解决的问题，从架构层面专门设计：

- **编译上下文。** 编译、建立索引和回答请求的每一步都知道文件在哪个上下文中编译——使用哪条编译命令，头文件由哪个文件包含——用户也可以查看并切换它。参见[编译上下文](compilation-context.md)。
- **多进程。** 主进程保存状态，worker 进程运行 Clang，因此 Clang 发生崩溃或内存泄漏只会损失一个 worker，不会拖垮整个服务器。参见[多进程架构](multi-process.md)。
- **统一的构建图。** 每一项高开销产物——Preamble 的 PCH、模块的 PCM、已打开文件的 AST、为建立索引而编译的翻译单元——都是同一张图中的节点，由这张图统一复用构建产物、取消构建，并沿依赖关系传播失效。参见[编译任务图](task-graph.md)。
- **协程。** 异步工作基于 kotatsu 库以 C++20 协程编写，不用回调。

## 分层结构

`src/` 是按层划分的单个 C++20 模块，下表自底向上列出各层：每一层只导入排在它前面的层，唯独 `server` 和 `analysis` 并列位于 `project` 之上，且 `analysis` 不导入 `worker`。

| 层         | 职责                                                                                                                                                          |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `support`  | 基础工具：日志、JSON、文本处理、模糊匹配、标记格式（markup）、失败记录。                                                                                      |
| `vfs`      | clice 的磁盘视图：同一文件无论路径如何书写都具有唯一标识；所有判断文件是否变化的地方共用同一套新鲜度检查（先做 stat，再算内容哈希）；此外还包含磁盘缓存存储。 |
| `command`  | 编译命令：从编译数据库解析一次，按规则编辑，再向每条命令的编译器查询以补全信息。参见[命令解析](command-resolve.md)。                                          |
| `syntax`   | 无需 AST 的语法处理：词法分析器、头文件包含与模块依赖扫描、依赖图、包含路径解析。参见[依赖扫描](dependency-scanning.md)。                                     |
| `compile`  | 统一接口背后的 Clang：单次编译的 AST、诊断、Token 与预处理指令，以及 clang-tidy。                                                                             |
| `semantic` | 基于 AST 的语义分析：每次编译遍历一次供所有功能查询、跨翻译单元一致的符号标识、[模板解析器](template-resolver.md)，以及文档注释。                             |
| `index`    | 符号索引：单翻译单元索引、按文件划分的分片（shard）、项目索引及其数据库，以及建立在它们之上的各类查询。参见[符号索引](symbol-index.md)。                      |
| `feature`  | 各项语言功能的响应结果，由 AST、索引或文档文本计算得出，自身不保存状态。                                                                                      |
| `settings` | `clice.toml` 及叠加其上的编辑器设置，以及决定每个文件适用哪些配置的规则。                                                                                     |
| `worker`   | worker 进程与进程池：任务分配、优先级、内存管理、崩溃处理和超时控制。                                                                                         |
| `project`  | 磁盘所呈现的项目状态：编译数据库、成员文件与编译命令、包含 PCH 和模块的构建图，以及项目索引。未保存的缓冲区从不改变项目对磁盘状态的描述。                     |
| `server`   | 语言服务器：已打开的文档、各文档归属的项目、当前由哪一方响应请求，以及与编辑器的连接。                                                                        |
| `analysis` | `clice analyze` 与 `clice modularize` 背后的分析逻辑。                                                                                                        |
| `driver`   | `clice` 可执行文件的各个子命令。                                                                                                                              |

## 数据流

```text
command (structured compile commands)
    ↓
project (task graph: PCH / PCM / AST / unit nodes) ──→ worker (processes)
    ↓ products
compile (drives Clang compilation)
    ↓
semantic (semantic map) ──→ index (unit index / shards / project index)
    ↓
feature (answers)
```

服务器将每个请求路由到当前能够作答的数据源——最新的 AST，或在文件编译期间改用索引——收集该功能所需的输入并返回响应。编译由请求按需拉动，单凭一次编辑从不直接触发编译（参见[增量编译](incremental-parse.md)）。
