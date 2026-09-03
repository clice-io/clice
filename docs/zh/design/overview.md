# 源码概览

本文档介绍 clice 源码中各模块的职责与定位，帮助读者建立对整个项目结构的整体认知。关于每个模块的详细设计，请参阅对应的专题文档。

## 项目愿景

clice 是一个全新的 C++ 语言服务器，从架构层面重新设计，旨在解决以往 C++ 语言服务器长期存在的问题。主要特点：

- **编译上下文**：clice 是首个将编译上下文作为正式概念引入的语言服务器。编译、索引和查询的每个步骤都会明确区分当前的编译上下文，用户可以查询和切换这些上下文。详见 [编译上下文](compilation-context.md)。

- **多进程架构**：master + worker 进程模型将 Clang 的内存泄漏和崩溃隔离开来，同时支持感知优先级的调度和实时内存监控。详见 [多进程架构](multi-process.md)。

- **协程异步模型**：基于 C++20 协程和 kotatsu 库，取代传统的回调式异步，使业务逻辑更清晰。

- **统一编译调度**：每个高开销的构建产物——Preamble PCH、C++20 模块 PCM、文档 AST、一次性批处理运行——都是统一编译任务图中的一个节点，支持构建共享、实时协作式取消和感知依赖关系的失效处理。详见 [编译任务图](task-graph.md)。

## 模块概览

### `src/support/` — 基础工具库

供其他所有模块共享的通用工具和基础设施。

- `CacheStore`：面向文件型构建产物（PCH/PCM 等）的磁盘制品存储——成对原子提交、命名空间配额、LRU 淘汰和崩溃恢复。
- `FuzzyMatcher`：用于代码补全和符号搜索的 Token 感知模糊匹配。
- Markup / Doxygen：文档注释的解析与格式化。
- 日志、文件系统抽象、字符串工具等。

### `src/vfs/` — 文件标识与版本

- `FileTable`：将文件路径映射为全系统使用的稳定 `Fid` 标识，并管理据此得到的各文件共享信息——stat 标记、内容版本、扫描结果和目录列表。两层时效性检查（先走 stat 快速路径，再计算内容哈希并修复 stat 标记）集中实现在此处，供所有使用方共享：PCH 验证、索引过期判断和磁盘轮询。

### `src/config/` — 配置

- `Config`：加载并合并 `clice.toml` 与 LSP `initializationOptions`，严格校验未知键，并包含用于生成配置文档的 JSON schema。

### `src/command/` — 编译命令处理

从编译数据库（CDB）读取的命令是构建系统生成的原始命令，不能直接交给 Clang 前端——它们可能包含仅用于代码生成的选项、缺少系统头文件搜索路径，或者包含与语言服务器无关的参数。本模块只需解析每条命令一次，即可将其转换为供系统其余部分使用的结构化形式。

- `CompilationDatabase`：加载 `compile_commands.json`，将每个条目解析一次，转换为由稳定 `ConfigID` 标识的、经过驻留和分类的参数结构。在此基础上，它会应用用户配置规则，为没有条目的文件合成备用命令，推导输入语言，计算条目标识哈希，并按需生成 driver 或 `-cc1` 命令行。
- `Toolchain`：查询系统编译器以获取隐式编译参数（例如系统头文件搜索路径），并采用两层缓存——按 driver 调用形式缓存探测结果，按输入类型缓存合成的选项集。
- `SearchConfig`：头文件搜索路径的四层模型（Quoted / Angled / System / After），与 Clang 内部的搜索逻辑一致。

详见 [编译命令解析](command-resolve.md)。

### `src/compile/` — 编译抽象

封装 Clang 编译器，将 Clang API 抽象为安全、统一的编译接口。这一层是纯粹的编译抽象，不包含任何服务器逻辑。

- `CompilationUnit` / `CompilationUnitRef`：Clang AST 上下文的 RAII 封装。`CompilationUnitRef` 提供统一的只读视图，可用于访问源位置映射、预处理器指令、AST 节点等。它是 `src/feature/` 和 `src/semantic/` 的主要输入。
- `CompilationParams`：描述单次编译的完整配置，包括编译类型（Preamble / Content / Completion / Indexing 等）、文件重映射、PCH/PCM 复用等。
- 在编译期间收集诊断信息和 clang-tidy 结果。

### `src/syntax/` — 轻量级语法处理

无需完整 AST 的语法级处理。在编译前运行，可快速获取文件的结构信息和依赖关系。

- `Lexer`：基于 Clang 原始词法分析器构建的 Token 级工具。不运行预处理器。用于扫描指令、解析 include 路径等。
- `DependencyGraph`：全局 include/模块依赖图，支持正向查询、反向查询、宿主源文件搜索、include 链查找等。
- 依赖扫描：封装 Clang 的 `DependencyDirectivesScanner`，快速提取 include 和模块依赖
- `IncludeResolver`：根据搜索路径配置将 include 路径解析为实际文件
- Preamble 合成：构建前缀/后缀文件，使头文件能够在其宿主文件的预处理器状态下编译

详见 [依赖扫描](dependency-scanning.md)。

### `src/semantic/` — 语义分析

提供 Clang 原生 API 之外的语义分析能力。以 `CompilationUnitRef` 为输入，提取更高层次的语义信息。

- `Semantics`：统一语义映射——每次编译仅遍历一次 AST，记录所有相关节点及其 Token 归属；选择、功能投影和索引生成都是对该映射的纯查询，而无须分别遍历 AST
- `TemplateResolver`：通过伪实例化解析依赖名，使语义分析能够透视模板上下文。详见 [模板解析器](template-resolver.md)。
- `SymbolKind` / `RelationKind`：细粒度的符号种类和关系类型

### `src/index/` — 符号索引

支持跨翻译单元查询的符号索引系统。层次结构如下：

- `TUIndex`：单次编译产出的索引行，从语义映射投影而来
- `ProjectIndex`：全局层——包含外部可见符号，以及翻译单元与其所贡献文件之间的对应记录
- `Shard`：以文件为单位的存储单元。每个以不同方式预处理该文件的编译上下文都会产生一个变体，这些变体根据其编码行的内容标识去重；文件局部名称保存在分片本身，只有外部名称会进入 `ProjectIndex`
- `BlobDatabase`：持久化后端——所有索引 blob 都存放在单个 LMDB 数据库中

详见 [符号索引](symbol-index.md)。

### `src/feature/` — LSP 功能实现

LSP 功能的具体实现。每个功能接收 `CompilationUnitRef`，返回对应的 LSP 响应数据。这一层是纯粹的计算层，不涉及网络通信、状态管理或进程调度。

包括：代码补全、悬停信息、签名帮助、语义高亮、内联提示、文档符号、文档链接、折叠范围、格式化、诊断等。

> `feature/` 仅涵盖单文件、基于 AST 的功能实现。跨文件代码导航功能（转到定义、查找引用等）由服务器的 `service/` 层（`Features`/`IndexQuery`）通过索引数据提供。部分功能涉及多阶段处理——例如，代码补全中的 include 路径补全无需完整编译即可在语法层处理。

### `src/sched/` — 编译调度核心

任务图引擎，负责决定构建哪些内容、何时构建，并共享构建结果。

- `TaskGraph`：共享构建图。每个计算开销较大的产物都是一个节点；对同一节点的并发请求会汇入同一轮构建，避免重复工作；取消采用协作式机制；图中的边会按依赖关系驱动失效处理
- `PCHFamily` / `PCMFamily` / `TURunFamily`：分别用于 Preamble PCH（以内容为键）、C++20 模块 PCM（包含导入边和提供方跟踪）以及索引与 lint 共用的一次性翻译单元运行的节点族
- `Workspace`：以磁盘状态为准的聚合体——包含编译数据库、依赖图、产物注册表和项目索引。核心不变量：打开文件中未保存的缓冲区内容绝不会修改 `Workspace`；它只反映磁盘上的状态
- `ContextResolver`：解析文件编译时采用的编译命令和包含方上下文，并负责头文件上下文判定、用户上下文选择和 Preamble 合成
- `IndexStore` / `IndexPump`：前者负责索引持久化事务（合并、保存、与 CDB 协调），后者是后台调度器，通过 worker 处理需要更新的文件，并采用感知前台负载的预算机制
- 引导与批处理驱动程序：负责服务器的冷启动编排，以及 `clice index` / `clice lint` 所使用的无头执行模式

### `src/worker/` — Worker 进程

- `WorkerPool`：管理 worker 进程的生命周期与调度——在崩溃预算约束下启动、监控和重新拉起进程，并在冷却后恢复；基于文档亲和性安排有状态任务；通过优先级队列和感知前台负载的容量管理分发无状态任务
- `StatefulWorker`：持有文档 AST 并响应查询请求
- 无状态 worker 执行一次性任务（PCH/PCM 构建、代码补全、格式化、索引任务）

### `src/server/` — 服务器运行时

语言服务器的核心运行时，负责将上述各层组装成可运行的服务。

**`protocol/`** — 协议定义。描述主进程与 worker 进程之间以及服务器与客户端之间通信所用的消息格式。包括 Worker 协议（编译/查询/构建请求）、LSP 扩展协议（编译上下文切换等），以及面向 AI agent 的 agentic 协议。

**`state/`** — 文档状态与失效机制。

- `Session` / `SessionStore`：记录每个打开文件的缓冲区权威状态——内容、文档版本、代次和服务状态——在 didOpen 时创建，在 didClose 时销毁。编译产物不存放在这里
- `ASTProjection` / `ASTProjectionTable`：每个文档最近一次编译所发布的产物（功能结果、PCH 键、依赖快照）——一种不可变的读取模型，每次发布时都会整体替换
- `Invalidator`：失效引擎——归并文件事件（缓冲区打开/保存、磁盘内容变化、编译数据库重新加载、worker 崩溃），生成一组去重的失效操作
- `FileTracker`：通过 stat 轮询发现编辑器之外发生的变化（重新生成 `compile_commands.json`、`git checkout`），并将事件送入 `Invalidator`
- `Quarantine`：按文档统计崩溃——内容屡次导致 worker 崩溃的文档会被隔离，并通过获准的探测尝试恢复

**`service/`** — 使用编译和索引结果的读取侧服务。

- `Features`：从各提供方——worker 的 AST、PCH 中缓存的 Preamble 产物或索引——组装各项功能的结果，并根据就绪状态路由每个请求：有最新 AST 时由 AST 响应，否则立即由索引支撑的投影响应。在 `readonly = on/auto` 下，文档完全由索引提供服务，直到编辑操作将其提升为完整的 AST 服务；编译始终按需进行，由请求而非生命周期事件触发
- `ASTFamily`：任务图中的文档 AST 节点族——调度打开文档的编译任务并发布结果
- `Dispatcher`：与 worker 通信的文档侧组件——每个携带打开文档内容的请求（有状态查询、交互式代码补全/签名帮助构建、格式化）都会先通过该文档的隔离检查，再进行分发，并统一经由一个出口返回；若客户端已修改该请求对应的缓冲区，则由该出口返回 `ContentModified`
- `IndexQuery`：对所有索引来源——项目索引、按文件划分的分片以及打开文件的实时数据——执行只读查询，并统一仲裁数据时效性；以领域值作答，再由各传输层映射到各自的协议
- `ContextService`：编译上下文查询与切换的协议适配器

**`transport/`** — 驱动服务器的协议端点。

- `MasterServer`：组合根。持有工作区、会话、工作线程池和上述所有服务，并通过自身唯一的分派入口执行 `Invalidator` 产生的操作
- `LSPClient` / `AgentClient`：LSP 协议和智能体协议的请求处理器

详见 [多进程架构](multi-process.md)。

### `src/driver/` — 子命令

`clice` 二进制程序的入口点：`serve`（LSP 服务器）、`worker`、`index`（批量索引）、`lint`（批量 clang-tidy）、`inspect`、`format`、`query` 和 `doc`。

## 模块间关系

数据流大致如下：

```text
command (structured compile commands)
    ↓
sched (task graph: PCH / PCM / AST / TU-run scheduling) ──→ worker (processes)
    ↓ products
compile (drives Clang compilation)
    ↓
semantic (semantic map) ──→ index (TUIndex / shards / ProjectIndex)
    ↓
feature (produces LSP responses)

The server layer assembles all of the above via sessions, routing,
invalidation, and transports into a runnable service.
```

`support`、`vfs`、`config` 和 `syntax` 是多个模块共享的横切层。
