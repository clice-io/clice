# Architecture

clice 采用**多进程**架构：一个主服务器（master）协调多个工作进程（worker）。主进程处理 LSP 请求、管理状态并分发编译任务；工作进程执行 CPU 密集型工作（解析、索引），运行在独立进程中并有内存限制。

异步运行时由 [kotatsu](https://github.com/clice-io/kotatsu)（`kota`）提供，它基于 libuv 封装了 C++20 协程。clice 不直接调用 libuv。

## 源码模块

### `src/command/`

CLI 解析、编译数据库加载与工具链检测。实现子命令式接口（`clice server`、`clice query` 等）。

### `src/compile/`

编译单元抽象。对 Clang 的 AST 和预处理器状态进行封装：

- `CompilationUnitRef` — 统一访问源码位置、预处理指令、文件内容
- `DiagnosticID` — 诊断分类（deprecated、unused 等）
- 预处理指令追踪（includes、embeds、模块声明）

### `src/feature/`

LSP 功能实现，每个功能消费一个 `CompilationUnitRef`：

- 代码补全、悬停信息、签名帮助
- 语义高亮、内嵌提示、文档符号
- 文档链接、折叠范围、格式化、诊断

### `src/index/`

符号索引，用于跨翻译单元的查询：

- `TUIndex` — 单翻译单元的符号数据
- `ProjectIndex` — 全局跨 TU 符号索引
- `MergedIndex` — 按文件分片，合并多个头文件上下文
- 包含关系图追踪

使用 FlatBuffers 序列化，高效磁盘存储。

### `src/semantic/`

超越 Clang 原生能力的语义分析：

- `SemanticVisitor` — AST 遍历，记录符号出现和关系
- `TemplateResolver` — 伪实例化，解析依赖名称
- `find_target` — 将节点解析到其声明目标
- 符号种类/修饰符分类

### `src/syntax/`

轻量级语法层处理（不需要完整 AST）：

- `Lexer`（`lexer.h`）与指令扫描（`scan.h`，封装 Clang 的 `DependencyDirectivesScanner`）— token 级别的工具
- `DependencyGraph` — 模块/包含依赖追踪
- 包含路径补全、模块导入补全

### `src/server/`

服务器核心，分为多个子模块：

#### `src/server/service/`

- `MasterServer` — 顶层 LSP 服务器，将请求路由到 session/worker
- `LSPClient` — LSP 协议处理，能力注册
- `Session` — 每文件状态（活跃上下文、待处理请求）

#### `src/server/compiler/`

- `Compiler` — 编译调度（PCH、PCM、索引构建）
- `CompileGraph` — 基于兴趣计数的模块 DAG，支持取消
- `Indexer` — 后台索引、查询解析

#### `src/server/worker/`

- `StatefulWorker` — 在内存中持有 AST，服务查询（hover、semantic tokens 等）
- `StatelessWorker` — 无状态任务（PCH/PCM 构建、补全、签名帮助）
- `WorkerPool` — 进程生命周期管理

#### `src/server/workspace/`

- `Workspace` — 项目状态、配置、依赖图
- `Config` — TOML/JSON 配置加载与校验

### `src/support/`

工具库：日志、文件系统辅助、JSON 序列化、字符串操作。

## 进程模型

```
┌─────────────────────────────────────────────────┐
│                    主进程 (Master)                 │
│  事件循环 (kota) + LSP I/O + 状态管理             │
└────────────┬──────────────────┬─────────────────┘
             │ bincode IPC      │ bincode IPC
     ┌───────▼───────┐   ┌─────▼──────────┐
     │ 有状态工作进程   │   │ 无状态工作进程   │
     │ (Stateful ×2) │   │ (Stateless ×N/2)│
     │ 持有 AST，     │   │ 构建 PCH/PCM，  │
     │ 服务查询       │   │ 补全、签名帮助   │
     └───────────────┘   └─────────────────┘
```

工作进程通过 stdin/stdout 使用 bincode 序列化与主进程通信。每个工作进程运行在独立进程中，可配置内存限制（默认 4 GB）。
