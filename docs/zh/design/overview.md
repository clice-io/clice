# 源码概览

本文档介绍 clice 源码中各模块的职责与定位，帮助读者建立对整个项目结构的初步认知。关于每个模块的详细设计，请参阅对应的专题文档。

## 项目定位

clice 是一个全新的 C++ 语言服务器，从架构层面重新设计，解决以往 C++ 语言服务器中长期存在的根本性问题。核心创新包括：

- **编译上下文（Compilation Context）**：同一个文件在不同的编译上下文下可能产生不同的结果。clice 将这一概念作为一等公民贯穿整个设计——从编译、索引到查询响应。现有的所有语言服务器（不仅是 C++）都没有正式处理这一概念。

- **多进程架构**：通过 master + worker 的进程模型隔离 Clang 的内存泄漏和崩溃问题，同时实现优先级调度和实时内存监控。

- **协程异步模型**：基于 C++20 协程和 kotatsu 库，取代传统的回调式异步，使业务逻辑更加清晰。

- **实时模块编译系统**：基于引用计数的 C++20 模块编译 DAG，支持实时取消和依赖级联，是目前已知的最优实时模块编译调度方案。

clice 的远期目标不仅是语言服务器，还将集成 clang-tidy 等工具的并发调度，实现跨翻译单元的优化（如头文件去重检查），成为 C++ 工具生态中统一的高级平台。

## 基础层

### `src/support/`

通用工具库。提供日志、文件系统抽象、字符串操作、模式匹配等基础设施。

关键组件：

- **路径池（PathPool）**：将文件路径内部化为紧凑的 `uint32_t` 标识符，在整个系统中用作文件的稳定标识。这是全局共享的基础设施，几乎所有涉及文件路径的模块都依赖它。
- **日志系统**：基于 spdlog 的结构化日志。
- **模糊匹配器（FuzzyMatcher）**：用于代码补全等场景的分词感知模糊匹配。
- **标记文档（Markup）/ Doxygen**：文档注释的解析与格式化。
- **去重集合（StringSet / ObjectSet）**：字符串和对象的去重内部化，通过 bump allocator 保证指针稳定性。

### `src/command/`

CLI 解析与编译命令处理。从编译数据库（CDB）读取的命令是构建系统生成的原始命令，不能直接交给 Clang 前端使用——它们可能包含仅用于代码生成的选项、缺少系统头文件搜索路径、或者包含语言服务器不需要的参数。这个模块负责对原始命令进行分类、过滤、工具链探测和去重，最终转化为语言服务器可消费的编译参数。

关键组件：

- **编译数据库（CompilationDatabase）**：加载 `compile_commands.json`，对编译命令进行两级分离——将影响语义的标志（canonical）与仅影响用户内容的标志（patch，如 `-I`、`-D`）分开，实现跨文件的命令去重与共享。
- **工具链（Toolchain）**：通过系统编译器查询完整的编译参数（如系统头文件搜索路径），按 (driver, file extension, non-user-content flags) 缓存查询结果，避免对同一工具链的重复探测。
- **参数分类器（ArgumentParser）**：将 Clang 编译选项分类为 codegen-only（丢弃）、discarded（丢弃）、user-content（提取为 patch）三类，确保只保留对语义分析有意义的参数。
- **搜索配置（SearchConfig）**：头文件搜索路径的四段式模型（Quoted / Angled / System / After），与 Clang 内部的搜索逻辑一致。

## 编译抽象层

### `src/compile/`

对 Clang 编译器的封装。将原始的 Clang API 抽象为安全、统一的编译接口，屏蔽底层细节。

这一层是纯粹的编译抽象，不涉及任何服务器逻辑。它接收编译参数，驱动 Clang 完成编译，产出编译结果——包括 AST、预处理器状态、源码位置映射等。

关键组件：

- **编译单元（CompilationUnit / CompilationUnitRef）**：对 Clang AST 上下文的 RAII 封装。`CompilationUnitRef` 提供统一的只读视图，用于访问源码位置映射、预处理指令、文件内容、AST 节点等。这是 `src/feature/` 和 `src/semantic/` 的主要输入。
- **编译参数（CompilationParams）**：描述一次编译操作的完整配置，包括编译类型（Preprocess / Content / Preamble / ModuleInterface / Completion / Indexing）、文件重映射、PCH/PCM 复用、取消标志等。
- **编译类型**：不同的编译类型产出不同的结果。Preamble 产出 PCH，ModuleInterface 产出 PCM，Content 产出完整 AST，Completion 产出补全候选项。它们共享同一套编译参数框架，但语义不同。

### `src/syntax/`

轻量级语法处理，不依赖完整 AST。

这一层处理那些不需要完整编译就能完成的任务：词法分析、依赖扫描、include 路径解析等。它运行在编译之前，用于快速获取文件的结构信息和依赖关系。

关键组件：

- **词法分析器（Lexer）**：基于 Clang 原始词法分析器（raw lexer）的 token 级别工具，提供流式接口。不经过预处理器，不展开宏。用于指令扫描、include 路径解析等场景。
- **依赖图（DependencyGraph）**：全局的 include / module 依赖关系图。记录每个文件（在每个 SearchConfig 下）的 include 关系，支持正向查询（文件包含了谁）、反向查询（谁包含了这个文件）、以及模块名到文件的映射。还提供 BFS 向上搜索宿主源文件、最短 include 链查找等导航功能。
- **依赖扫描（Scan）**：封装 Clang 的 `DependencyDirectivesScanner`，快速提取文件的 include 和 module 依赖而不需要完整预处理。
- **Include 解析器（IncludeResolver）**：根据搜索路径配置解析 include 路径到实际文件。
- **补全（Completion）**：include 路径补全和 module 导入补全的候选项生成。

## 语义分析层

### `src/semantic/`

超越 Clang 原生 API 的语义分析能力。

这一层接收 `CompilationUnitRef`，从中提取更高层次的语义信息——符号关系、符号分类、模板解析等。它的产出是索引系统和 LSP 功能的核心数据来源。

关键组件：

- **语义访问器（SemanticVisitor）**：AST 遍历器，记录每个符号的出现位置和关系（定义、引用、读、写、继承、调用等）。这是索引数据的主要生产者。
- **模板解析器（TemplateResolver）**：通过伪实例化技术解析依赖名称（dependent names），使语义分析能穿透模板上下文。这是 clice 的一项关键创新，clangd 在此领域能力有限。
- **符号分类（SymbolKind / RelationKind）**：细粒度的符号种类和关系类型，远比 LSP 协议定义的更加精细。
- **目标解析（find_target）**：将 AST 节点解析到其声明目标，处理各种间接引用和隐式转换。

## 索引层

### `src/index/`

符号索引系统，支持跨翻译单元的查询。

索引系统采用三级结构，对应不同的查询场景和生命周期：

- **TU 索引（TUIndex）**：单次编译产出的索引数据。记录编译涉及的每个文件中的符号出现、关系和 include 图。这是索引的原始数据来源，由 `SemanticVisitor` 在编译过程中生成。
- **项目索引（ProjectIndex）**：全局符号表。汇聚所有已索引文件的符号信息，支持按符号哈希查询名称、种类和引用文件。用于跨文件的符号搜索和导航。
- **合并索引（MergedIndex）**：按文件分片的索引存储。将同一文件在不同编译上下文下产生的索引数据合并存储。合并索引是编译上下文感知的核心体现——一个头文件可能被多个源文件包含，每次包含产生不同的符号关系，合并索引将它们统一起来。

索引使用 FlatBuffers 序列化，支持磁盘持久化和惰性加载。

## 功能层

### `src/feature/`

LSP 功能的具体实现。每个功能接收一个 `CompilationUnitRef`（或 `CompilationParams`），返回对应的 LSP 响应数据。

这一层是纯粹的计算层——不涉及网络通信、状态管理或进程调度。它只关心"给定一个编译结果，如何产出正确的 LSP 响应"。包括：

- 代码补全、悬停信息、签名帮助
- 语义高亮、内嵌提示
- 文档符号、文档链接
- 折叠范围、格式化、诊断

每个功能实现为一个独立函数，遵循统一的模式。新增 LSP 功能时，参考已有实现即可。

> **注意**：`feature/` 只涵盖单文件、基于 AST 的特性实现。跨文件的语言特性（如 go to definition、find references、call hierarchy 等）由 `src/server/compiler/` 中的 Indexer 基于索引数据完成。此外，部分特性存在多阶段处理流程，并不一定走到 AST 层——例如代码补全在某些场景下可以在语法层（include 路径补全、module 导入补全）直接完成，document links 也可能在预处理阶段就能提取。`feature/` 中的实现仅对应"需要完整编译结果"的那个阶段。

## 服务器层

### `src/server/`

语言服务器的核心运行时。这是最复杂的一层，负责将上述所有层组装成一个可运行的服务。

#### `src/server/protocol/`

协议定义。描述主进程与工作进程之间、以及与客户端之间的通信消息格式。

- **Worker 协议**：定义有状态和无状态工作进程的请求/响应消息——编译请求、查询请求、构建请求、文档更新通知等。使用 bincode 序列化。
- **扩展协议（Extension）**：LSP 标准之外的扩展请求，如编译上下文查询与切换。
- **Agentic 协议**：面向 AI agent 的高级查询接口，提供编译命令查询、项目文件列表、文件依赖分析、影响分析、符号搜索、符号详情读取等功能。这些接口通过命令行直接访问，不需要 MCP 等中间层。

#### `src/server/workspace/`

项目级全局状态——来自磁盘的唯一真相源。

- **工作区（Workspace）**：持有编译数据库、工具链、路径池、依赖图、PCH/PCM 缓存、项目索引等全部项目级状态。核心不变量：打开文件的未保存缓冲区内容不会修改 Workspace。Workspace 的状态变更来自三个路径：初始化加载、文件保存（didSave）触发的级联更新、以及后台索引完成后的索引合并。
- **配置（Config）**：TOML/JSON 配置文件的加载与校验。

#### `src/server/compiler/`

编译调度与索引管理。这一层是 `src/compile/`（底层编译抽象）和 `src/server/service/`（服务层）之间的桥梁。

- **编译器（Compiler）**：编译生命周期的调度器。协调 PCH 构建、模块依赖解析、AST 编译的先后顺序，将编译任务分发给工作进程。它本身不持有持久数据——所有数据都存在 Workspace 和 Session 中，Compiler 只负责编排执行流程。
- **编译图（CompileGraph）**：C++20 模块编译的 DAG 调度器。基于引用计数实现兴趣追踪——当没有任何请求者关心某个模块单元时，自动取消其编译。支持依赖级联取消和文件更新触发的重编译。
- **索引器（Indexer）**：后台索引的调度和跨文件查询的入口。管理索引队列、去重、空闲超时批处理。查询时综合三个数据源：ProjectIndex 用作目录（定位符号所在文件），MergedIndex 提供按文件分片的关系数据，打开文件的内存索引提供未保存的实时状态覆盖。

#### `src/server/service/`

服务入口与会话管理。

- **主服务器（MasterServer）**：顶层协调器。持有 Workspace、Session 映射、WorkerPool、Compiler、Indexer，将 LSP 请求路由到对应的处理逻辑。管理服务器生命周期（Uninitialized → Initialized → Ready → ShuttingDown → Exited），启动文件监视任务。
- **会话（Session）**：每个打开文件的编辑状态。持有当前缓冲区内容、编译版本号、ABA 防护的 generation 计数器、内存符号索引、PCH 引用、依赖快照等。Session 在 didOpen 时创建，didClose 时销毁。Session 的修改不影响其他文件——所有跨文件依赖指向磁盘文件。
- **LSP 客户端（LSPClient）**：LSP 协议的请求处理器，注册各 LSP 方法的 handler。
- **Agent 客户端（AgentClient）**：Agentic 协议的请求处理器。

#### `src/server/worker/`

工作进程管理。

- **工作池（WorkerPool）**：管理工作进程的生命周期、路由和调度。
  - **有状态工作进程（StatefulWorker）**：在内存中持有 AST，服务查询请求（hover、semantic tokens 等）。每个打开的文件通过 path_id 亲和性绑定到一个有状态工作进程，新文件分配给当前负载最轻的工作进程。
  - **无状态工作进程（StatelessWorker）**：执行一次性任务（PCH/PCM 构建、补全、索引等）。采用优先级感知调度——交互式请求（补全、签名帮助）优先于后台任务（索引）。并发数量根据内存压力和工作进程崩溃情况动态调整。
- **进程容错**：工作进程崩溃时自动重启（有最大重启次数限制），有状态工作进程崩溃时自动重新分配其拥有的文档。

## 模块间关系

数据流方向大致为：

```
command（编译命令解析）
    ↓
compile（驱动 Clang 编译）
    ↓
semantic（提取语义信息） ──→ index（构建索引）
    ↓
feature（产出 LSP 响应）

server 层将以上所有层通过 Workspace/Session/WorkerPool 组装成可运行的服务
```

`support` 和 `syntax` 是横切层，被多个模块共享。`server/protocol` 定义了进程间和客户端间的消息契约。
