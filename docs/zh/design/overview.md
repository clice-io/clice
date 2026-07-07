# 源码概览

本文档介绍 clice 源码中各模块的职责与定位，帮助读者建立对整个项目结构的初步认知。关于每个模块的详细设计，请参阅对应的专题文档。

## 项目定位

clice 是一个全新的 C++ 语言服务器，从架构层面重新设计，解决以往 C++ 语言服务器中长期存在的问题。主要特点：

- **编译上下文**：clice 是第一个将编译上下文作为正式概念引入的语言服务器。编译、索引、查询的每一步都明确区分当前使用的编译上下文，用户可以查询和切换。详见 [编译上下文](compilation-context.md)。

- **多进程架构**：通过 master + worker 的进程模型隔离 Clang 的内存泄漏和崩溃问题，同时实现优先级调度和实时内存监控。详见 [多进程架构](multi-process.md)。

- **协程异步模型**：基于 C++20 协程和 kotatsu 库，取代传统的回调式异步，使业务逻辑更清晰。

- **实时模块编译**：基于引用计数的 C++20 模块编译 DAG，支持实时取消和依赖级联。详见 [模块编译图](module-graph.md)。

## 模块概览

### `src/support/` — 基础工具库

通用工具和基础设施，被其他所有模块共享。

- `PathPool`：将文件路径内部化为 `uint32_t` 标识符，在整个系统中用作文件的稳定标识
- `FuzzyMatcher`：分词感知的模糊匹配，用于代码补全和符号搜索
- Markup / Doxygen：文档注释的解析与格式化
- 日志、文件系统抽象、字符串工具等

### `src/command/` — 编译命令处理

从编译数据库（CDB）读取的命令是构建系统生成的原始命令，不能直接交给 Clang 前端使用——可能包含仅用于代码生成的选项、缺少系统头文件搜索路径、或包含语言服务器不需要的参数。这个模块负责对原始命令进行分类、过滤、工具链探测和去重，转化为语言服务器可消费的编译参数。

- `CompilationDatabase`：加载 `compile_commands.json`，将编译命令分为影响语义的标志（canonical）和仅影响用户内容的标志（patch，如 `-I`、`-D`），实现跨文件的命令去重与共享
- `Toolchain`：通过系统编译器查询完整的编译参数（如系统头文件搜索路径），按 (driver, file extension, non-user-content flags) 缓存查询结果
- `SearchConfig`：头文件搜索路径的四段式模型（Quoted / Angled / System / After），与 Clang 内部的搜索逻辑一致

详见 [编译命令解析](command-resolve.md)。

### `src/compile/` — 编译抽象

对 Clang 编译器的封装，将 Clang API 抽象为安全、统一的编译接口。这一层是纯粹的编译抽象，不涉及任何服务器逻辑。

- `CompilationUnit` / `CompilationUnitRef`：对 Clang AST 上下文的 RAII 封装。`CompilationUnitRef` 提供统一的只读视图，用于访问源码位置映射、预处理指令、AST 节点等，是 `src/feature/` 和 `src/semantic/` 的主要输入
- `CompilationParams`：描述一次编译的完整配置，包括编译类型（Preamble / Content / Completion / Indexing 等）、文件重映射、PCH/PCM 复用等

### `src/syntax/` — 轻量级语法处理

不依赖完整 AST 的语法层处理，运行在编译之前，用于快速获取文件的结构信息和依赖关系。

- `Lexer`：基于 Clang raw lexer 的 token 级别工具，不经过预处理器，用于指令扫描、include 路径解析等
- `DependencyGraph`：全局的 include / module 依赖关系图，支持正向查询、反向查询、宿主源文件搜索、include 链查找等
- 依赖扫描：封装 Clang 的 `DependencyDirectivesScanner`，快速提取 include 和 module 依赖
- `IncludeResolver`：根据搜索路径配置解析 include 路径到实际文件

详见 [依赖扫描](dependency-scanning.md)。

### `src/semantic/` — 语义分析

超越 Clang 原生 API 的语义分析能力。接收 `CompilationUnitRef`，提取更高层次的语义信息。

- `SemanticVisitor`：AST 遍历器，记录每个符号的出现位置和关系（定义、引用、继承、调用等），是索引数据的主要生产者
- `TemplateResolver`：通过伪实例化技术解析依赖名称（dependent names），使语义分析能穿透模板上下文。详见 [模板解析器](template-resolver.md)
- `SymbolKind` / `RelationKind`：细粒度的符号种类和关系类型

### `src/index/` — 符号索引

符号索引系统，支持跨翻译单元的查询。采用三级结构：

- `TUIndex`：单次编译产出的索引数据，由 `SemanticVisitor` 生成
- `ProjectIndex`：全局符号表，汇聚所有已索引文件的符号信息，支持按符号哈希查询
- `MergedIndex`：按文件分片的索引存储，将同一文件在不同编译上下文下产生的索引数据合并

详见 [符号索引](symbol-index.md)。

### `src/feature/` — LSP 功能实现

LSP 功能的具体实现。每个功能接收一个 `CompilationUnitRef`，返回对应的 LSP 响应数据。这一层是纯粹的计算层，不涉及网络通信、状态管理或进程调度。

包括：代码补全、悬停信息、签名帮助、语义高亮、内嵌提示、文档符号、文档链接、折叠范围、格式化、诊断等。

> `feature/` 只涵盖单文件、基于 AST 的特性实现。跨文件的导航功能（go to definition、find references 等）由服务器的 `service/` 层（`FeatureRouter`/`IndexQuery`）基于索引数据提供。部分功能存在多阶段处理——例如代码补全中的 include 路径补全在语法层就能完成，不需要完整编译。

### `src/server/` — 服务器运行时

语言服务器的核心运行时，负责将上述所有层组装成一个可运行的服务。

**`protocol/`** — 协议定义。描述主进程与工作进程之间、以及与客户端之间的通信消息格式。包括 Worker 协议（编译/查询/构建请求）、LSP 扩展协议（编译上下文切换等）、以及面向 AI agent 的 agentic 协议。

**`state/`** — 项目与文档状态，以及失效机制。

- `Workspace`：项目级全局状态——编译数据库、工具链、路径池、依赖图、缓存存储、项目索引。核心不变量：打开文件的未保存缓冲区内容不会修改 `Workspace`，它只反映磁盘上的状态
- `Session` / `SessionStore`：每个打开文件的编辑状态（缓冲区内容、文档版本号、内存索引、PCH 引用等），在 didOpen 时创建，didClose 时销毁；`SessionStore` 拥有打开会话表和缓冲区同步逻辑
- `Invalidator`：失效引擎——把文件事件（打开/保存、磁盘变化、编译数据库重载、工作进程崩溃）折叠成一组去重后的失效效果
- `FileTracker`：以 stat 轮询发现编辑器之外发生的变化（重新生成的 `compile_commands.json`、`git checkout`），把事件交给 `Invalidator`
- `Config`：`clice.toml` 与 LSP `initializationOptions` 的加载与合并

**`compiler/`** — 编译调度与索引管理。

- `Compiler`：编译生命周期的调度器，协调 PCH 构建、模块依赖解析、AST 编译的先后顺序，将编译任务分发给工作进程
- `CompileGraph`：C++20 模块编译的 DAG 调度器，基于引用计数实现兴趣追踪，支持依赖级联取消
- `ContextResolver`：解析文件的编译命令与 includer 上下文，拥有头文件上下文状态与合成 preamble
- `Indexer`：后台索引调度——将过期文件入队、把索引构建分发给工作进程并合并产出的分片

**`service/`** — 消费编译与索引结果的读侧服务。

- `FeatureRouter`：把 feature 请求路由到编译器（依赖 AST 的功能）或索引（跨文件导航），必要时合并两者
- `IndexQuery`：`ProjectIndex`、`MergedIndex` 与打开文件内存索引之上的查询门面

**`transport/`** — 驱动服务器的协议端点。

- `MasterServer`：组合根。持有 `Workspace`、`SessionStore`、`WorkerPool` 及上述全部服务，并通过唯一的 dispatch 入口执行 `Invalidator` 的失效效果
- `LSPClient` / `AgentClient`：LSP 协议和 agentic 协议的请求处理器

**`worker/`** — 工作进程管理。

- `WorkerPool`：管理工作进程的生命周期和调度。有状态工作进程（`StatefulWorker`）持有 AST 服务查询请求，无状态工作进程（`StatelessWorker`）执行一次性任务（PCH/PCM 构建、补全、索引等）
- 进程容错：工作进程崩溃时自动重启，有状态工作进程崩溃时自动重新分配文档

详见 [多进程架构](multi-process.md)。

## 模块间关系

数据流方向大致为：

```text
command（编译命令解析）
    ↓
compile（驱动 Clang 编译）
    ↓
semantic（提取语义信息） ──→ index（构建索引）
    ↓
feature（产出 LSP 响应）

server 层将以上各层通过 Workspace / Session / WorkerPool 组装成可运行的服务
```

`support` 和 `syntax` 是横切层，被多个模块共享。
