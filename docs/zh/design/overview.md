# 源码概览

本文档介绍 clice 源码中各模块的职责与定位，帮助读者建立对整个项目结构的整体认知。关于每个模块的详细设计，请参阅对应的专题文档。

## 项目定位

clice 是一个全新的 C++ language server，从架构层面重新设计，解决以往 C++ language server 中长期存在的问题。主要特点：

- **编译上下文**：clice 是第一个将编译上下文作为正式概念引入的 language server。编译、索引、查询的每一步都明确区分当前使用的编译上下文，用户可以查询和切换。详见 [编译上下文](compilation-context.md)。

- **多进程架构**：master + worker 进程模型隔离 Clang 的内存泄漏和崩溃，同时实现优先级感知调度和实时内存监控。详见 [多进程架构](multi-process.md)。

- **协程异步模型**：基于 C++20 协程和 kotatsu 库，取代传统的回调式异步，使业务逻辑更清晰。

- **统一编译调度**：每个昂贵的构建产物——preamble PCH、C++20 模块 PCM、文档 AST、一次性批处理运行——都是单一编译任务图中的一个节点，具备共享构建、实时协作取消和依赖感知失效。详见 [编译任务图](task-graph.md)。

## 模块概览

### `src/support/` — 基础工具库

通用工具和基础设施，被其他所有模块共享。

- `CacheStore`：面向文件型构建产物（PCH/PCM 等）的磁盘制品存储——原子双提交、命名空间配额、LRU 淘汰和崩溃恢复。
- `FuzzyMatcher`：针对代码补全和符号搜索的分词感知模糊匹配。
- Markup / Doxygen：文档注释的解析与格式化。
- 日志、文件系统抽象、字符串工具等。

### `src/vfs/` — 文件标识与版本

- `FileTable`：把文件路径内部化为全系统使用的稳定 `Fid` 标识，并持有由它们推导出的共享按文件事实——stat 戳、内容版本、扫描结果、目录列表。两层新鲜度检查（先走 stat 快速路径，再做内容哈希与戳修复）只在此实现一次，供所有消费者共享：PCH 校验、索引陈旧判断和磁盘轮询。

### `src/config/` — 配置

- `Config`：加载并合并 `clice.toml` 与 LSP `initializationOptions`，进行严格的未知键校验，并生成配置文档所用的 JSON schema。

### `src/command/` — 编译命令处理

从编译数据库（CDB）读取的命令是构建系统生成的原始命令，不能直接交给 Clang 前端使用——它们可能包含仅用于代码生成的选项、缺少系统头文件搜索路径，或者包含与 language server 无关的参数。本模块将每条命令解析一次，转换成系统其余部分消费的结构化形式。

- `CompilationDatabase`：加载 `compile_commands.json`，把每个条目解析一次，转换成由稳定 `ConfigID` 标识的驻留、分类参数结构。在此基础上，它应用用户配置规则，为没有条目的文件合成备用命令，推导输入语言，计算条目身份哈希，并按需渲染 driver 或 `-cc1` 命令行。
- `Toolchain`：查询系统编译器以获取隐式编译参数（例如系统头文件搜索路径），采用两层缓存——按 driver 调用形态缓存探测结果，按输入类型缓存合成的标志集。
- `SearchConfig`：头文件搜索路径的四层模型（Quoted / Angled / System / After），与 Clang 内部的搜索逻辑一致。

详见 [编译命令解析](command-resolve.md)。

### `src/compile/` — 编译抽象

封装 Clang 编译器，将 Clang API 抽象为安全、统一的编译接口。这一层是纯粹的编译抽象，不包含任何服务器逻辑。

- `CompilationUnit` / `CompilationUnitRef`：围绕 Clang AST 上下文的 RAII 封装。`CompilationUnitRef` 提供统一的只读视图，用于访问源码位置映射、预处理指令、AST 节点等。这是 `src/feature/` 和 `src/semantic/` 的主要输入。
- `CompilationParams`：描述单次编译的完整配置，包括编译类型（Preamble / Content / Completion / Indexing 等）、文件重映射、PCH/PCM 复用等。
- 编译期间的诊断和 clang-tidy 收集。

### `src/syntax/` — 轻量级语法处理

不需要完整 AST 的语法层处理。在编译之前运行，快速获取文件的结构信息和依赖关系。

- `Lexer`：基于 Clang raw lexer 的 token 级工具。不经过预处理器。用于指令扫描、include 路径解析等。
- `DependencyGraph`：全局 include/module 依赖图，支持正向查询、反向查询、宿主源文件搜索、include 链查找等。
- 依赖扫描：封装 Clang 的 `DependencyDirectivesScanner`，快速提取 include 和 module 依赖
- `IncludeResolver`：根据搜索路径配置将 include 路径解析为实际文件
- preamble 合成：构建前缀/后缀文件，使头文件能在其宿主的预处理器状态下编译

详见 [依赖扫描](dependency-scanning.md)。

### `src/semantic/` — 语义分析

超越 Clang 原生 API 的语义分析能力。接收 `CompilationUnitRef`，提取更高层次的语义信息。

- `Semantics`：统一的语义映射——每次编译只做一次 AST 遍历，记录所有感兴趣的节点及其 token 归属；selection、feature 投影和索引产出都是对该映射的纯粹查询，而不是各自单独遍历 AST
- `TemplateResolver`：通过伪实例化解析依赖名称，使语义分析能够穿透模板上下文。详见 [模板解析器](template-resolver.md)。
- `SymbolKind` / `RelationKind`：细粒度的符号种类和关系类型

### `src/index/` — 符号索引

支持跨翻译单元查询的符号索引系统。层次结构如下：

- `TUIndex`：单次编译产出的索引行，从语义映射投影而来
- `ProjectIndex`：全局层——外部可见符号，以及哪些翻译单元贡献了哪些文件的记录
- `Shard`：按文件存储的单元。每个以不同方式预处理该文件的编译上下文都会贡献一个变体，按编码行的内容标识去重；文件内局部名称保存在 shard 本身，只有外部名称进入 `ProjectIndex`
- `BlobDatabase`：持久化后端——所有索引 blob 都存放在单个 LMDB 数据库中

详见 [符号索引](symbol-index.md)。

### `src/feature/` — LSP 功能实现

LSP 功能的具体实现。每个功能接收 `CompilationUnitRef`，返回对应的 LSP 响应数据。这一层是纯粹的计算层，不涉及网络通信、状态管理或进程调度。

包括：代码补全、hover 信息、签名帮助、语义高亮、内嵌提示、文档符号、文档链接、折叠范围、格式化、诊断等。

> `feature/` 只涵盖单文件、基于 AST 的功能实现。跨文件导航功能（go to definition、find references 等）由服务器的 `service/` 层（`FeatureRouter`/`IndexQuery`）基于索引数据提供。部分功能涉及多阶段处理——例如，代码补全中的 include 路径补全可以在语法层完成，无需完整编译。

### `src/sched/` — 编译调度核心

决定构建什么、何时构建并共享结果的任务图引擎。

- `TaskGraph`：共享的构建图。每个昂贵的构建产物都是一个节点；对同一节点的并发请求会合并到一轮构建中，而不是重复工作；取消是协作式的；边驱动依赖感知的失效
- `PCHFamily` / `PCMFamily` / `TURunFamily`：节点族，分别对应 preamble PCH（以内容为键）、C++20 模块 PCM（带 import 边和提供者跟踪），以及由索引和 lint 共享的一次性翻译单元运行
- `Workspace`：磁盘真值聚合——编译数据库、依赖图、产物注册表和项目索引。核心不变量：打开文件的未保存缓冲区内容绝不会修改 `Workspace`；它只反映磁盘上的状态
- `ContextResolver`：解析文件编译时使用的编译命令和 includer 上下文，拥有头文件上下文判定、用户上下文选择和合成的 preamble
- `IndexStore` / `IndexPump`：索引持久化事务（合并、保存、与 CDB 协调）以及后台调度器，后者通过 worker 处理过期文件，并带有前台感知的预算控制
- Bootstrap 与批量驱动程序：服务器的冷启动编排，以及 `clice index` / `clice lint` 背后的无头执行模式

### `src/worker/` — Worker 进程

- `WorkerPool`：管理 worker 进程生命周期与调度——启动/监控/重启带崩溃预算和冷却恢复，按文档亲和性做有状态放置，以及带优先级队列和前台感知容量的无状态分发
- `StatefulWorker`：持有文档 AST 并为查询请求提供服务
- 无状态 worker 执行一次性任务（PCH/PCM 构建、补全、格式化、索引运行）

### `src/server/` — 服务器运行时

语言服务器的核心运行时，负责将上述各层组装成一个可运行的服务。

**`protocol/`** — 协议定义。描述 master 进程与 worker 进程之间，以及服务器与客户端之间的通信消息格式。包括 Worker 协议（编译/查询/构建请求）、LSP 扩展协议（编译上下文切换等），以及面向 AI agent 的 agentic 协议。

**`state/`** — 文档状态与失效机制。

- `Session` / `SessionStore`：每个打开文件的打开缓冲区真相——内容、文档版本、代数、服务状态——在 didOpen 时创建，在 didClose 时销毁。编译产物不存放在这里
- `ASTProjection` / `ASTProjectionTable`：每个文档最近一次编译的已发布产物（功能结果、PCH key、依赖快照）——一个不可变的读取模型，在每次发布时整体替换
- `Invalidator`：失效引擎——将文件事件（缓冲区打开/保存、磁盘变化、编译数据库重载、worker 崩溃）折叠为一组去重后的失效效果
- `FileTracker`：通过 stat 轮询发现编辑器之外发生的变化（重新生成的 `compile_commands.json`、`git checkout`），并将事件送入 `Invalidator`
- `Quarantine`：按文档的崩溃记账——内容持续导致 worker 崩溃的文档会被隔离，并通过受控的探测尝试恢复

**`service/`** — 消费编译与索引结果的读侧服务。

- `FeatureRouter`：按就绪程度路由每个 feature 请求——有最新的 AST 时由 AST 回答，否则由索引支持的投影立即回答。在 `readonly = on/auto` 下，文档完全从索引提供服务，直到编辑将其升级为完整的 AST 服务；编译始终是 pull 驱动的，由请求触发而非生命周期事件
- `ASTFamily`：任务图中的文档 AST 节点族——为打开文档调度编译并发布其结果
- `WorkerForwarder`：将 feature 工作分发给 worker（有状态查询、交互式补全/签名构建、格式化），并带有隔离准入和结果落地
- `IndexQuery`：项目索引、按文件分片以及打开文件实时数据之上的查询门面
- `ContextService`：编译上下文查询与切换的协议适配器

**`transport/`** — 驱动服务器的协议端点。

- `MasterServer`：组合根。持有 workspace、sessions、worker pool 以及上述所有服务，并通过其唯一的 dispatch 入口执行 `Invalidator` 的失效效果
- `LSPClient` / `AgentClient`：LSP 协议和 agentic 协议的请求处理器

详见 [多进程架构](multi-process.md)。

### `src/driver/` — 子命令

`clice` 二进制的入口点：`serve`（LSP 服务器）、`worker`、`index`（批量索引）、`lint`（批量 clang-tidy）、`inspect`、`format`、`query` 和 `doc`。

## 模块间关系

数据流大致遵循以下方向：

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

`support`、`vfs`、`config` 和 `syntax` 是横切层，被多个模块共享。
