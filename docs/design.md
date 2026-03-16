# clice 详细设计文档

clice 的完整设计文档，涵盖设计动机、系统分层、各类职责、IPC 协议定义、状态管理、编译系统等。

---

## 0. 设计动机与总体方针

### 0.1 为什么选多进程

与 clangd 的多线程方案不同，clice 选择**多进程**进行并行编译，核心原因如下：

- **clang parser 在解析不完整代码时频繁 crash。** clang 本身经过长期测试，在处理完整代码时很稳定；但语言服务器需要频繁解析用户正在编辑的、不完整的代码，这是 clangd 99% crash 的来源。
- **多进程隔离崩溃。** 单个 Worker 进程 crash 只需重启该进程，不影响主进程和其他 Worker，用户几乎无感知。
- **缓解 clang 的内存泄漏问题。** 通过监控子进程内存用量，可以及时终止异常进程并启动新进程。
- **与 clangd 的关键区别：** clangd 不在多次启动之间保存任何状态，crash 后需要重新解析大量内容，用户体验很差。clice 通过持久化缓存（PCH/PCM/索引）大幅降低了 crash 的恢复成本。

### 0.2 进程池策略

- 在 Linux 上启动进程很快，开销可忽略；但在 Windows 上进程启动耗时较大。
- 因此 clice 采用**常驻进程池**：每个进程可执行多个任务，仅在崩溃或被主动 kill 时才启动新进程，避免频繁创建进程的开销。

### 0.3 核心框架

- 采用 **eventide** 库作为异步层和通信框架。
- 主进程与 LSP Client 之间使用 JSON-RPC（文本格式）通信。
- 主进程与 Worker 之间使用 JSON-RPC（bincode 格式）通信，减少序列化开销。
- 两种通信方式均由 eventide 框架直接提供支持。

---

## 1. 系统总体分层

```
┌─────────────────────────────────────────────────────┐
│                   LSP Client (IDE)                  │
│               stdin/stdout 或 TCP                    │
└──────────────────────┬──────────────────────────────┘
                       │ JSON-RPC (text)
                       ▼
┌─────────────────────────────────────────────────────┐
│                 MasterServer (主进程)                 │
│                                                     │
│  ┌──────────┐ ┌───────────┐ ┌────────────────────┐  │
│  │文档状态管理│ │编译调度系统 │ │全局索引 & 包含图管理│  │
│  └──────────┘ └───────────┘ └────────────────────┘  │
│  ┌──────────┐ ┌───────────┐ ┌────────────────────┐  │
│  │LRU 文档池 │ │CDB 管理   │ │配置文件 & 监听      │  │
│  └──────────┘ └───────────┘ └────────────────────┘  │
│                                                     │
│  StatelessWorkerPool          StatefulWorkerPool    │
│  ┌────────┬────────┐         ┌────────┬────────┐   │
│  ▼        ▼        ▼         ▼        ▼        ▼   │
│ SL-0    SL-1    SL-N        SF-0    SF-1    SF-M   │
└──┼────────┼────────┼──────────┼────────┼────────┼──┘
   │        │        │          │        │        │
   │  JSON-RPC (bincode)       │  JSON-RPC (bincode)
   ▼        ▼        ▼         ▼        ▼        ▼
┌────────────────────────┐  ┌────────────────────────┐
│  StatelessWorker 进程   │  │  StatefulWorker 进程    │
│  (无状态, 一次性编译)    │  │  (持有 AST, 连续遍历)   │
└────────────────────────┘  └────────────────────────┘
```

### 1.1 进程边界

| 进程                      | 通信方式                                      | 序列化格式                |
| ------------------------- | --------------------------------------------- | ------------------------- |
| LSP Client ↔ MasterServer | stdin/stdout (pipe 模式) 或 TCP (socket 模式) | JSON-RPC 2.0 (文本)       |
| MasterServer ↔ Worker     | stdin/stdout (子进程管道)                     | JSON-RPC (bincode 二进制) |

### 1.2 层次职责划分

| 层次        | 目录            | 职责                                                           |
| ----------- | --------------- | -------------------------------------------------------------- |
| Server 层   | `src/server/`   | 进程管理、LSP 协议处理、请求路由、文档状态、LRU 管理           |
| Compile 层  | `src/compile/`  | 编译参数构建、编译执行、CDB 管理、Preamble/PCH/PCM、clang-tidy |
| Feature 层  | `src/feature/`  | 各 LSP 功能实现（hover、completion、diagnostics 等）           |
| Semantic 层 | `src/semantic/` | AST 工具函数、模板解析、选择树、符号分类                       |
| Index 层    | `src/index/`    | 符号索引、包含图、TU 索引、项目索引、序列化                    |
| Syntax 层   | `src/syntax/`   | 依赖指令扫描（词法/模糊/精确三级）、Lexer、Token 表示          |
| Support 层  | `src/support/`  | 日志、文件系统、Doxygen、模糊匹配、Glob 等通用工具             |

---

## 2. 启动流程

```
main()
  │
  ├─ 解析 CLI 参数 → Options
  │
  ├─ if mode == Stateless Worker:
  │     run_stateless_worker_mode()
  │       └─ 创建 event_loop → 打开 stdio → 创建 Peer
  │          → StatelessWorker(peer) → loop.run()
  │
  ├─ if mode == Stateful Worker:
  │     run_stateful_worker_mode()
  │       └─ 创建 event_loop → 打开 stdio → 创建 Peer
  │          → StatefulWorker(peer, doc_capacity) → loop.run()
  │
  ├─ if mode == Pipe:
  │     run_pipe_mode()
  │       └─ 创建 event_loop → 打开 stdio → run_master_session()
  │
  └─ if mode == Socket:
        run_socket_mode()
          └─ 创建 event_loop → TCP listen → accept → run_master_session()

run_master_session():
    创建 Peer → MasterServer(loop, peer, options)
    → StatelessWorkerPool.start() (spawn 无状态 worker 进程)
    → StatefulWorkerPool.start() (spawn 有状态 worker 进程)
    → 注册 LSP 回调
    → peer.run() + loop.run()
```

### 2.1 端到端启动序列

从 `initialize` 请求到第一个文件可用的完整流程：

```
initialize 请求到达
  │
  ├─ 1. 解析 workspace root (从 initialize params 获取)
  ├─ 2. 加载 clice.toml (workspace 根目录)
  │       └─ 确定 CDB 路径、缓存路径、功能配置等
  ├─ 3. 确定缓存目录 (<workspace>/.clice/ 或 clice.toml 覆盖)
  ├─ 4. spawn Worker 进程池
  │       ├─ StatelessWorkerPool.start()
  │       └─ StatefulWorkerPool.start()
  ├─ 5. 加载 CDB (compile_commands.json)
  │       └─ 获取所有源文件列表和编译命令
  ├─ 6. 尝试加载缓存
  │       ├─ project.idx → ProjectIndex (全局符号表)
  │       ├─ shards/*.idx → MergedIndex 分片 (lazy 反序列化)
  │       └─ graph.idx → FuzzyGraph (模糊包含图)
  ├─ 7. 返回 initialize 响应 (server capabilities)
  │
  └─ initialized 通知到达后:
        ├─ 若 graph.idx 缓存不存在:
        │     异步启动 FuzzyGraph 全量扫描
        │     (scan_fuzzy 遍历 CDB 所有文件, 共享 SharedScanCache)
        │     → 发送 LSP Progress "Scanning includes..."
        ├─ 启动 CDB 文件 watcher (监听 compile_commands.json 变更)
        ├─ 启动 clice.toml 文件 watcher
        └─ 初始化空闲索引调度器

didOpen (首个文件到达)
  │
  ├─ 创建 DocumentState (version, text, generation)
  ├─ 查询 CDB 获取编译命令
  │     └─ 若 CDB 中无此文件: 使用启发式默认编译参数
  ├─ 扫描文件的 import/include 依赖
  ├─ compile_graph.register_unit(path, dependencies)
  └─ schedule_build(uri) → run_build_drain:
        ├─ co_await compile_graph.compile(path)
        │     └─ 递归编译 PCM/PCH 依赖 (通过 StatelessWorker)
        │     └─ 发送 LSP Progress "Building preamble..."
        ├─ 发送 worker::CompileParams 给 StatefulWorker
        ├─ 收到 CompileResult (含 diagnostics)
        └─ 发布 diagnostics 给 LSP client
            → 文件现在可用，后续 feature request 可正常处理
```

---

## 3. MasterServer 详细设计

### 3.1 核心状态

```cpp
// 每个打开文档的状态
struct DocumentState {
    int version;           // LSP 文档版本
    std::string text;      // 文档全文
    uint64_t generation;   // 递增计数器，每次内容变更 +1
    bool build_running;    // 是否有 build 任务正在执行
    bool build_requested;  // 是否有待执行的 build 请求
};
```

**generation 的作用**：用于检测过期请求。当一个异步请求（如 hover）返回时，比较请求发起时的 generation 和当前 generation，若不一致则丢弃结果。这是 priority-aware lazy push 设计的关键。

### 3.2 生命周期状态机

```
                 ┌─ InitializeRequest ──┐
                 │                      ▼
[未初始化] ──────┘               [已初始化]
                                     │
                              InitializedNotification
                                     │
                                     ▼
                               [就绪运行中]
                                     │
                              ShutdownRequest
                                     │
                                     ▼
                               [关闭中]
                                     │
                              ExitNotification
                                     │
                                     ▼
                               [已退出]
```

状态守卫规则：
- 未收到 `initialize` 前，拒绝所有请求
- 收到 `shutdown` 后，拒绝新请求，但继续处理已有任务
- 收到 `exit` 后，关闭 worker 池，停止事件循环

### 3.3 文本同步模式

LSP 客户端使用 **incremental text sync** 模式：`didChange` 只传输增量 diff，主进程在本地应用 diff 维护 `DocumentState.text` 全量文本。发送给 Worker 的 `worker::CompileParams` 和 `worker::DocumentUpdateParams` 传输全量 text（Worker 需要完整源文件进行编译）。

选择 incremental sync 的原因：
- 减少 Client → Server 的传输量（大文件编辑时尤为明显）
- 增量 diff 携带编辑位置信息，可用于判断用户编辑频率和位置
- **未来优化**：根据用户频繁编辑的区域动态调整 preamble bound（扩充 PCH 覆盖范围）

### 3.4 文档事件处理 (Priority-Aware Lazy Push)

核心原则：**区分"当前文件"和"依赖文件"，采用不同的编译触发策略**。对当前正在编辑的文件主动 push 编译结果，对其他文件延迟到 `didSave` 或用户访问时再处理。

**优先级排序**：

| 优先级 | 文件类别 | 触发策略 |
|--------|---------|---------|
| 1 (最高) | 当前正在编辑/查看的文件 | `didChange` → 立即 push 重建 AST + diagnostics |
| 2 | 最近访问过的文件 | debounced push |
| 3 | 其他打开的文件 | lazy on-demand（收到 feature request 时才编译） |
| 4 (最低) | 后台索引文件 | 空闲时编译 |

**当前编辑文件（didChange，未保存）**：

```
didChange
    │
    ├─ 更新 DocumentState (version, text, generation++)
    ├─ 设置 build_requested = true
    ├─ compile_graph.update(path)  ← 仅标脏 + 取消正在编译的依赖链
    └─ schedule_build(uri)
           │
           └─ 若 build_running == false:
                  启动 run_build_drain 协程
```

**依赖当前文件的其他文件（未收到 didSave）**：
- 只标 dirty，**不主动重建 AST**
- 等到 `didSave` 消息到达后，才触发依赖文件的 debounced rebuild
- 或者等到用户切换到那个文件（feature request 触发 on-demand 编译）

**didSave 触发依赖重建**：

```
didSave
    │
    ├─ 更新 DocumentState
    ├─ compile_graph.compile(path)  ← 重建依赖链（PCM/PCH）
    └─ 对依赖此文件的已打开文档：schedule_build(dependent_uri)
```

**didClose 处理**：

```
didClose
    │
    ├─ 移除 DocumentState
    ├─ 取消该文件的 build_drain（如果正在运行）
    ├─ 不立即驱逐 StatefulWorker 的 AST（LRU 自然淘汰）
    │     （用户可能切回来，保留 AST 减少重建开销）
    └─ 发布空 diagnostics 清除编辑器中该文件的诊断信息
```

**与 CompileGraph 的关系**：
- `didChange` 只调用 `compile_graph.update(path)` 标脏 + 取消正在编译的依赖链
- `didSave` 才触发 `compile_graph.compile()` 重建依赖链
- 当前文件的 PCH/PCM 依赖如果已就绪则直接用，不因未保存的编辑而重建依赖

**Build Drain Loop**（`run_build_drain`）：

```
while true:
    if doc not found: return
    if !build_requested: build_running=false; return
    build_requested = false
    记录当前 generation

    // 先确保 PCM/PCH 依赖就绪（通过 CompileGraph）
    co_await compile_graph.compile(path)
    if 依赖编译失败或被取消: continue

    // 发送编译请求给 StatefulWorker（携带完整编译上下文）
    发送 worker::CompileParams 到 worker
    等待 worker::CompileResult
    if 编译失败: continue (忽略错误，等下次请求)
    if doc 已关闭: return
    if generation 已过期: continue (文档已更新，重新编译)
    发布 diagnostics 给 client
```

这保证了：
1. 连续快速编辑只触发最后一次编译
2. 编译结果与当前文档版本一致时才发布
3. 编译失败不阻塞后续请求
4. 依赖文件不会因为未保存的编辑频繁重建
5. PCM/PCH 依赖在编译主文件前已就绪

### 3.4 请求处理流程

以 Hover 请求为例：

```
on_hover(params)
    │
    ├─ 状态检查 (initialized && !shutdown)
    ├─ 查找 DocumentState
    ├─ 创建 Snapshot {uri, version, generation, text, line, character}
    └─ run_hover(snapshot)
           │
           ├─ ensure_compiled(uri)  ← 确保该文件已编译
           │       │
           │       ├─ if build_running: co_await 当前 build drain 完成
           │       ├─ elif 文件未编译 (低优先级文件, on-demand):
           │       │     co_await compile_graph.compile(path)  ← 编译依赖
           │       │     co_await stateful_workers.compile(params)  ← 编译主文件
           │       └─ 文件已编译: 继续
           │
           ├─ 构造 worker::HoverParams {uri, line, character}
           ├─ co_await workers.hover(params)
           │       │
           │       ├─ assign_worker(uri)  ← LRU + 负载均衡
           │       ├─ send_request → worker (Worker 已持有 AST)
           │       └─ 若 worker 崩溃: restart_worker, 重试一次
           │
           ├─ 检查 generation 是否过期
           └─ 返回结果 (或 nullopt)
```

**on-demand 编译路径**：对于低优先级文件（优先级 3），用户切换到该文件时发出的首个 feature request 会触发主进程的 on-demand 编译。这仍然由**主进程驱动**——主进程先通过 CompileGraph 编译依赖，再发送 `clice/worker/compile` 给 Worker。Worker 不会自行发起编译。

---

## 4. WorkerPool 详细设计

主进程维护两个独立的 Worker 池：`StatelessWorkerPool` 和 `StatefulWorkerPool`。

### 4.1 StatelessWorkerPool

无状态 Worker 池不维护 URI → Worker 映射，任务可以发给任意 Worker。主进程根据任务优先级选择目标 Worker：高优先级任务（PCM/PCH/Completion/SignatureHelp）发给通用 Worker，低优先级任务（Index）尽量发给专用的后台索引 Worker。

### 4.2 StatefulWorkerPool 分配策略

有状态 Worker 池维护 URI → Worker 的映射，同一文档的请求尽量发给同一 Worker（保留 AST 缓存）。

```cpp
assign_worker(uri):
    if uri 已有 owner:
        touch LRU
        return owner[uri]

    shrink_owner()  // 若 worker 内存超限，淘汰最久未用的
    selected = pick_worker()  // 选择 owned_documents 最少的 worker
    owner[uri] = selected
    workers[selected].owned_documents++
    touch LRU
    return selected
```

**负载均衡**：选择当前持有文档数最少的 worker（贪心策略）。

### 4.3 LRU 淘汰机制

主进程维护全局 LRU（`owner_lru` + `owner_lru_index`），管理 StatefulWorkerPool 中 URI → Worker 的映射：

- **容量**：基于内存用量动态决定。由于单个 AST 的典型内存占用为 1\~2 GB，固定文档数上限并不合理；主进程监控各 StatefulWorker 的内存用量，当总内存超过阈值时淘汰最久未用的文档。
- **淘汰时机**：`assign_worker` 时若内存超限，或周期性检查触发
- **淘汰动作**：向 StatefulWorker 发送 `worker::EvictParams` notification，Worker 释放缓存的文档和 AST

StatefulWorker 内部也维护独立 LRU，同样基于内存用量管理自身缓存：

```
主进程监控内存  ──超限──→  worker::EvictParams  ──→  StatefulWorker 内部释放
StatefulWorker 内部 LRU  ──超限──→  自动释放最久未用文档
                                               └──→  发送 clice/worker/evicted 通知主进程
```

**Worker 驱逐回报**：当 StatefulWorker 内部 LRU 主动驱逐文档时，必须发送 `clice/worker/evicted` 通知给主进程。主进程收到后清除对应的 `owner` 映射，避免主进程认为 Worker 仍持有该文档的 AST 而导致的状态漂移。

### 4.4 Worker 崩溃恢复

两种 Worker 池共用相同的崩溃恢复策略：

```
worker request 失败
    │
    ├─ restart_worker(index)
    │     ├─ spawn 新 worker 进程
    │     ├─ SIGTERM 旧进程
    │     └─ schedule reap_worker_process (异步回收)
    │
    └─ 重试一次请求
         └─ 若仍失败: 返回 unexpected error
```

对于 StatefulWorker：崩溃后主进程**必须清除该 Worker 的所有 `owner` 映射**。新 Worker 启动后没有旧 AST，若不清除 owner 映射，主进程会错误地认为新 Worker 仍持有旧文档。清除后，后续请求会重新通过 `assign_worker` 分配，主进程会重新发送完整文档内容和编译请求。

对于 StatelessWorker：无状态，崩溃后只需重启进程、重试任务即可。

---

## 5. Worker 详细设计

Worker 分为两种完全不同的类型，运行在独立进程中，各自有不同的生命周期和状态管理策略。

### 5.1 StatelessWorker（无状态 Worker）

无状态 Worker 不保存任何跨请求的资源。它的职责是接收一次性编译任务，执行完毕后销毁 AST，返回结果。

**处理的任务**（按优先级降序）：

| 优先级 | 任务 | 输入 | 产出 |
|--------|------|------|------|
| 1 | Build PCM | 模块源文件 + 编译参数 | 磁盘上的 .pcm 文件 |
| 2 | Build PCH | 头文件 + 编译参数 | 磁盘上的 .pch 文件 |
| 3 | CodeCompletion | 源文件 + 光标位置 | CompletionList |
| 4 | SignatureHelp | 源文件 + 光标位置 | SignatureHelp |
| 5 | Index | 源文件 + 编译参数 | TUIndex |

**内部设计**：

```cpp
class StatelessWorker {
    jsonrpc::Peer& peer;
    // 无任何文档缓存或 AST 状态

    // 收到请求 → 投递到线程池 → 编译 → 返回结果 → 销毁 AST
    // cancellation: 使用 Peer 框架提供的 cancellation token
    //               编译过程中轮询 token，请求取消时自动退出
};
```

StatelessWorker 内部不需要关心任务优先级，所有任务投递到线程池执行即可。优先级调度由主进程负责：主进程尽量将低优先级任务（如 Index）分发到单独的 StatelessWorker 进程，并调低该进程的调度优先级，实现后台索引。

**取消机制**：eventide Peer 框架直接提供 cancellation token。当主进程取消请求时，token 被设置，Worker 线程池中正在执行的编译任务通过轮询该 token 检测到取消并退出。因此 StatelessWorker 的代码可以设计得极为简单。

### 5.2 StatefulWorker（有状态 Worker）

有状态 Worker 持有已编译文档的 AST，用于处理需要连续遍历 AST 的请求。

**处理的任务**：

| 任务 | 说明 |
|------|------|
| Parse (编译主文件) | 收到文档更新后重新编译，产生 AST 和 Diagnostics |
| Hover | 基于 AST 查询符号信息 |
| SemanticTokens | 基于 AST 遍历产生语义高亮 |
| InlayHints | 基于 AST 产生内联提示 |
| FoldingRange | 基于 AST + 预处理指令产生折叠范围 |
| DocumentSymbol | 基于 AST 遍历产生文档符号树 |
| DocumentLink | 基于包含图产生可点击的 #include 链接 |
| CodeAction | 基于 AST + diagnostics 产生代码操作 |
| SelectionRange | 基于 AST SelectionTree 产生选择范围（v2，v1 暂不实现） |

**并发模型**：事件循环 + 线程池，per-document task queue (strand 模式) 串行化。

- **事件循环线程**负责接收请求、管理状态（LRU、document map）
- **AST 遍历任务和编译任务**提交到线程池执行
- 同一文档的任务通过 **per-document task queue（strand 模式）** 串行化：每个 `DocumentEntry` 维护一个任务队列，编译任务和 AST 遍历任务依次弹出执行，保证同一文档的所有操作串行
- 不同文档的 task queue 互相独立，可在线程池中并行执行
- 编译任务（compile 请求）同样在线程池执行，通过 strand queue 自动串行化

**核心约束**：clang AST 遍历**非线程安全**，同一 AST 的所有遍历任务必须串行执行。不同文档的 AST 互相独立，可以并行遍历。strand 模式天然满足此约束。

**内部状态**：

```cpp
class StatefulWorker {
    jsonrpc::Peer& peer;
    std::size_t memory_limit;       // 内存用量上限 (bytes)

    struct DocumentEntry {
        int version;
        std::string text;
        uint64_t generation;        // 文档更新计数
        CompilationUnit unit;       // 持有的 AST（可能为空）
        bool dirty;                 // AST 是否过期

        // 编译参数（由主进程通过 worker::CompileParams 传入）
        std::string directory;
        std::vector<std::string> arguments;
        std::pair<std::string, uint32_t> pch;
        StringMap<std::string> pcms;
    };

    std::unordered_map<std::string, DocumentEntry> documents;
    // LRU 管理 (list + index)
    std::list<std::string> lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_index;
};
```

**主进程驱动的被动编译模型**：

StatefulWorker **不自行决定编译时机**。主进程通过 `run_build_drain` 全流程协调：先通过 CompileGraph 编译 PCM/PCH 依赖，然后发送 `clice/worker/compile` 请求给 StatefulWorker，Worker 被动执行编译。

```
主进程 run_build_drain:
    │
    ├─ co_await compile_graph.compile(path)  ← 确保 PCM/PCH 依赖就绪
    └─ 发送 worker::CompileParams 给 StatefulWorker
         （携带 directory, arguments, pch, pcms 等完整编译上下文）

StatefulWorker 收到 compile 请求:
    │
    ├─ 更新 DocumentEntry: version, text, 编译参数
    ├─ 提交编译任务到线程池（per-document 串行化）
    ├─ 编译文档 → 产生新 AST + diagnostics
    ├─ 编译完成后通知事件循环更新状态
    ├─ 持有 AST 供后续 feature request 使用
    └─ 返回 worker::CompileResult (含 diagnostics)

文档更新通知到达 (documentUpdate):
    │
    ├─ 更新 DocumentEntry: version, text, generation++
    ├─ 标记 dirty = true
    └─ 若有正在进行的 AST 遍历任务: 标记其 cancellation token

功能请求到达 (e.g., Hover):
    │
    ├─ 查找对应 DocumentEntry
    ├─ 提交到 per-document strand queue
    │     （主进程保证 compile 请求先于 feature request 发出，
    │      因此 strand queue 中 compile 任务一定排在 feature 前面，
    │      Worker 无需额外等待逻辑）
    ├─ 使用 AST 执行功能请求
    │     （遍历期间若 dirty 变为 true → 取消遍历，返回 cancelled）
    │
    └─ 返回结果
```

**编译结果同时产生 Diagnostics**：StatefulWorker 编译文档后，除了持有 AST，还会将 diagnostics 作为编译结果的一部分返回给主进程，主进程再发布给 LSP client。

**Worker 驱逐回报**：当 StatefulWorker 内部 LRU 驱逐文档时，主动发送 `clice/worker/evicted` 通知给主进程，以便主进程清除对应的 owner 映射，避免状态漂移。

**LRU 淘汰**：

```cpp
void shrink_if_over_limit() {
    while (current_memory_usage() > memory_limit && !lru.empty()) {
        // 淘汰最久未用的文档
        // 释放其 CompilationUnit (AST)
        // 取消该文档上正在进行的任务
    }
}

void evict_document(uri) {
    // 收到主进程的 worker::EvictParams
    // 释放文档缓存和 AST
    // 取消该文档上正在进行的任务
}
```

### 5.3 两种 Worker 的进程管理

```
MasterServer
├── StatelessWorkerPool
│     ├─ Worker 0 (通用)
│     ├─ Worker 1 (通用)
│     └─ Worker N (低优先级，专用于后台索引)
│
└── StatefulWorkerPool
      ├─ Worker 0  ── owns {doc_a, doc_b, ...}
      ├─ Worker 1  ── owns {doc_c, doc_d, ...}
      └─ Worker M  ── owns {doc_e, doc_f, ...}
```

**关键区别**：
- StatelessWorkerPool：任务可以发给任意 worker，无 URI 亲和性
- StatefulWorkerPool：使用 LRU + 负载均衡的 URI → Worker 映射，同一文档的请求尽量发给同一 worker（保留 AST 缓存）

**进程启动参数区别**：

```
# 无状态 worker
clice --worker --stateless

# 有状态 worker
clice --worker --stateful --worker-memory-limit=4G
```

---

## 6. IPC 协议定义

### 6.1 Master ↔ Worker 协议

所有 IPC 消息通过 eventide 的 JSON-RPC bincode 传输。

#### StatefulWorker 请求 (Request/Response, Master → StatefulWorker)

| 方法名                           | Params                         | Result                         | 描述                       |
| -------------------------------- | ------------------------------ | ------------------------------ | -------------------------- |
| `clice/worker/compile`           | `worker::CompileParams`        | `worker::CompileResult`        | 编译文档，返回 diagnostics |
| `clice/worker/hover`             | `worker::HoverParams`          | `worker::HoverResult`          | 查询 hover 信息            |
| `clice/worker/semanticTokens`    | `worker::SemanticTokensParams` | `worker::SemanticTokensResult` | 语义高亮                   |
| `clice/worker/inlayHints`        | `worker::InlayHintsParams`     | `worker::InlayHintsResult`     | 内联提示                   |
| `clice/worker/foldingRange`      | `worker::FoldingRangeParams`   | `worker::FoldingRangeResult`   | 折叠范围                   |
| `clice/worker/documentSymbol`    | `worker::DocumentSymbolParams` | `worker::DocumentSymbolResult` | 文档符号                   |
| `clice/worker/documentLink`      | `worker::DocumentLinkParams`   | `worker::DocumentLinkResult`   | 文档链接                   |
| `clice/worker/codeAction`        | `worker::CodeActionParams`     | `worker::CodeActionResult`     | 代码操作                   |
| `clice/worker/goToDefinition`   | `worker::GoToDefinitionParams` | `worker::GoToDefinitionResult` | Go To Definition (AST fallback) |

#### StatelessWorker 请求 (Request/Response, Master → StatelessWorker)

| 方法名                           | Params                         | Result                         | 描述                       |
| -------------------------------- | ------------------------------ | ------------------------------ | -------------------------- |
| `clice/worker/completion`        | `worker::CompletionParams`     | `worker::CompletionResult`     | 代码补全                   |
| `clice/worker/signatureHelp`     | `worker::SignatureHelpParams`  | `worker::SignatureHelpResult`  | 签名帮助                   |
| `clice/worker/buildPCH`          | `worker::BuildPCHParams`       | `worker::BuildPCHResult`       | 构建预编译头               |
| `clice/worker/buildPCM`          | `worker::BuildPCMParams`       | `worker::BuildPCMResult`       | 构建预编译模块             |
| `clice/worker/index`             | `worker::IndexParams`          | `worker::IndexResult`          | 索引文件                   |

#### 通知 (Notification，无响应)

| 方法名                        | Params                         | 方向            | 描述                                 |
| ----------------------------- | ------------------------------ | --------------- | ------------------------------------ |
| `clice/worker/evict`          | `worker::EvictParams`          | Master → Worker | 通知 worker 释放指定文档缓存         |
| `clice/worker/documentUpdate` | `worker::DocumentUpdateParams` | Master → Worker | 通知文档内容更新（不请求结果）       |
| `clice/worker/evicted`        | `worker::EvictedParams`        | Worker → Master | Worker 内部 LRU 驱逐文档时通知主进程 |

> **`documentUpdate` 发送条件**：`documentUpdate` 只发送给已有 owner 的文档。如果文档尚未被分配 Worker（从未编译过），主进程仅在本地更新 `DocumentState`，不发送 `documentUpdate`。Worker 会在首次收到 `worker::CompileParams`（含完整 text）时获得文档内容。

#### 消息结构定义

```cpp
namespace worker {

// === StatefulWorker 编译 (Master → StatefulWorker) ===

// 编译文档，携带完整编译上下文
struct CompileParams {
    std::string uri;
    int version;
    std::string text;
    std::string directory;              // 编译命令工作目录
    std::vector<std::string> arguments; // 编译参数
    std::pair<std::string, uint32_t> pch; // PCH 产物路径 + preamble bound（空路径=无PCH）
    StringMap<std::string> pcms;        // module_name → pcm_path
};
struct CompileResult {
    std::string uri;
    int version;
    std::vector<Diagnostic> diagnostics;
};

// 文档更新通知 (Master → StatefulWorker)
struct DocumentUpdateParams {
    std::string uri;
    int version;
    std::string text;
};

// Hover — Worker 已持有 AST，只需 uri + position
struct HoverParams {
    std::string uri;
    int line;
    int character;
};
struct HoverResult {
    std::optional<Hover> result;
};

// SemanticTokens
struct SemanticTokensParams {
    std::string uri;
};
struct SemanticTokensResult {
    SemanticTokens result;
};

// InlayHints
struct InlayHintsParams {
    std::string uri;
    Range range;
};
struct InlayHintsResult {
    std::vector<InlayHint> result;
};

// FoldingRange
struct FoldingRangeParams {
    std::string uri;
};
struct FoldingRangeResult {
    std::vector<FoldingRange> result;
};

// DocumentSymbol
struct DocumentSymbolParams {
    std::string uri;
};
struct DocumentSymbolResult {
    std::vector<DocumentSymbol> result;
};

// DocumentLink
struct DocumentLinkParams {
    std::string uri;
};
struct DocumentLinkResult {
    std::vector<DocumentLink> result;
};

// CodeAction
struct CodeActionParams {
    std::string uri;
    Range range;
    CodeActionContext context;
};
struct CodeActionResult {
    std::vector<CodeAction> result;
};

// GoToDefinition — AST fallback (主进程索引查询未命中时)
struct GoToDefinitionParams {
    std::string uri;
    int line;
    int character;
};
struct GoToDefinitionResult {
    std::vector<Location> result;
};

// === StatelessWorker 消息 (Master → StatelessWorker) ===

// Completion
struct CompletionParams {
    std::string uri;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch; // PCH 路径 + preamble bound
    StringMap<std::string> pcms;          // module_name → pcm_path
    int line;
    int character;
};
struct CompletionResult {
    std::optional<CompletionList> result;
};

// SignatureHelp
struct SignatureHelpParams {
    std::string uri;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch; // PCH 路径 + preamble bound
    StringMap<std::string> pcms;          // module_name → pcm_path
    int line;
    int character;
};
struct SignatureHelpResult {
    std::optional<SignatureHelp> result;
};

// Build PCH
struct BuildPCHParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string content;
};
struct BuildPCHResult {
    bool success;
    std::string error;
};

// Build PCM
struct BuildPCMParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string module_name;
    StringMap<std::string> pcms; // name → path
};
struct BuildPCMResult {
    bool success;
    std::string error;
};

// Index
struct IndexParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    StringMap<std::string> pcms; // name → path
};
struct IndexResult {
    bool success;
    std::string error;
    std::string tu_index_data; // FlatBuffers serialized TUIndex (opaque bytes)
};

// === 共用通知 ===

// Evict (Master → Worker)
struct EvictParams {
    std::string uri;
};

// Worker 内部驱逐回报 (Worker → Master)
struct EvictedParams {
    std::string uri;
};

} // namespace worker
```

### 6.2 逻辑协议与线协议

6.1 节的结构体定义是**逻辑协议**——描述每个消息的语义字段和类型，用于理解接口契约。实际的**线协议 (wire protocol)** 有两种模式：

**模式 1：透明转发（大部分 Feature Request）**

主进程与 Worker 之间使用 bincode JSON-RPC。对于可透明转发的请求（Worker 结果即最终结果），Worker 将 result 序列化为 **opaque JSON 字符串**，通过 bincode 传回主进程。主进程**不反序列化**该字符串，直接嵌入 JSON-RPC response 原样转发给 LSP Client。

```
Worker                        MasterServer                    LSP Client
  │                               │                               │
  │ bincode response              │                               │
  │ { result: "<JSON文本>" }  ──→ │                               │
  │  (string = 长度 + 内容)       │ 取出 <JSON文本>，              │
  │                               │ peer.write() 直接写入          │
  │                               │ JSON-RPC response ─────────→  │
```

在这种模式下，Worker 端的 result 类型实际上是 `std::string`（opaque JSON），而非 6.1 节定义的强类型结构体。6.1 节的强类型定义描述的是这个 JSON 字符串的**内容语义**。

**模式 2：结构化解析（需要合并的请求）**

当主进程需要合并 Worker 结果与自身查询结果时（如 FindReferences 合并索引查询 + AST 分析），主进程会反序列化 Worker 的响应为强类型结构体。此时 6.1 节的类型定义直接对应实际的反序列化目标。

只有少量请求使用模式 2，透明转发是默认路径。

| 请求类型 | 是否可透明转发 | 说明 |
|---------|--------------|------|
| Hover | 是 | Worker 结果即最终结果 |
| Completion | 是 | Worker 结果即最终结果 |
| SignatureHelp | 是 | Worker 结果即最终结果 |
| SemanticTokens | 是 | Worker 结果即最终结果 |
| InlayHints | 是 | Worker 结果即最终结果 |
| FoldingRange | 是 | Worker 结果即最终结果 |
| DocumentSymbol | 是 | Worker 结果即最终结果 |
| DocumentLink | 是 | Worker 结果即最终结果 |
| Find References | 否 | 需合并索引查询 + AST 分析 |
| Rename | 否 | 需合并索引查询 + AST 分析 |
| Call Hierarchy | 否 | 需合并索引查询 |
| Type Hierarchy | 否 | 需合并索引查询 |

### 6.3 协议完整性说明

以上 6.1 节已定义所有 v1 版本的 IPC 消息，涵盖 StatefulWorker（compile、hover、semanticTokens、inlayHints、foldingRange、documentSymbol、documentLink、codeAction）和 StatelessWorker（completion、signatureHelp、buildPCH、buildPCM、index）的全部请求，以及双向通知（evict、documentUpdate、evicted）。

PrepareRename 和 WorkspaceSymbol 在主进程直接处理（分别查询 MergedIndex 和 ProjectIndex），不需要转发到 Worker，因此不涉及 IPC 消息。SelectionRange 推迟到 v2 实现，v1 暂不定义对应 IPC。

---

## 7. 并发与安全性

### 7.1 线程安全保证

| 组件             | 线程模型               | 约束                         |
| ---------------- | ---------------------- | ---------------------------- |
| MasterServer     | 单线程事件循环 (libuv) | 所有状态访问在事件循环线程   |
| StatelessWorker  | 单线程事件循环 + 线程池 | 编译任务在线程池执行         |
| StatefulWorker   | 事件循环 + 线程池（per-document 串行化） | 同一文档的 AST 遍历串行化，不同文档可并行 |
| LRU 缓存         | 归属进程的单线程       | 无并发访问                   |
| 索引查询         | 主进程线程池并行读     | 只读访问通过 atomic swap 隔离，合并在事件循环线程独占执行 |

### 7.2 索引 Merge 与 Query 的并发隔离

**方案：Atomic Swap**

MergedIndex 内部当前使用 `std::unique_ptr<llvm::MemoryBuffer>` 持有磁盘数据。为支持线程安全的 snapshot 读取，需将 `buffer` 字段升级为 `std::shared_ptr<llvm::MemoryBuffer>`，查询线程通过 `std::atomic_load` 获取当前 buffer 的 `shared_ptr` 副本：

```
查询线程 (线程池)                    事件循环线程 (merge)
      │                                    │
      ├─ atomic_load(shared_ptr)           │
      ├─ 在 buffer 上查询                   │
      │  (只读, 天然线程安全)               │
      │                                    ├─ load_in_memory() → Impl
      │                                    ├─ 修改 Impl (merge 新数据)
      │                                    ├─ serialize() → new buffer
      │                                    └─ atomic_store(shared_ptr, new_buffer)
      │                                         │
      ├─ 查询完成, 释放旧 shared_ptr          │
      │  (旧 buffer 在最后引用者释放后自动销毁) │
```

- 查询线程通过 `atomic_load` 获取 `shared_ptr` 副本，在 buffer 上直接查询
- Merge 操作在事件循环线程：`load_in_memory()` → 修改 `Impl` → `serialize()` 到新 buffer → `atomic_store` 替换 `shared_ptr`
- 查询线程持有的旧 `shared_ptr` 在最后一个查询结束后自动释放
- 这比 read-write lock 更简单，且不会阻塞查询线程

> **注**：当前代码使用 `unique_ptr<MemoryBuffer>`，实现时需升级为 `shared_ptr` 以支持上述并发 snapshot 读取。

### 7.3 取消传播

```
用户编辑文档
  → MasterServer: generation++, schedule_build
  → Build Drain: 检测到新 generation，丢弃旧编译结果
  → Worker 内: cancellation token 被设置，编译轮询退出

用户切换文件 / 关闭文件
  → MasterServer: release_document
  → worker::EvictParams 发送给 worker
  → Worker: evict_document，释放缓存和 AST
```

### 7.4 崩溃恢复

1. Worker 崩溃 → MasterServer 检测到请求失败
2. `restart_worker`: SIGTERM 旧进程 + spawn 新进程
3. **对 StatefulWorker：清除该 Worker 的所有 owner 映射**（参见 4.4 节）
4. 重试一次请求（新 worker 会收到完整文档内容和编译上下文）
5. 若重试仍失败 → 返回错误给 LSP client（client 通常会静默处理）

### 7.5 取消机制对照

系统中有三种取消机制，分别适用于不同层级：

| 取消机制 | 适用位置 | 使用场景 | 原理 |
|---------|---------|---------|------|
| `et::cancellation_source/token` | 主进程 CompileGraph | 编译依赖图的级联取消 | 通过 `with_token()` 绑定到 co_await，`source.cancel()` 取消所有关联任务 |
| `shared_ptr<atomic_bool> stop` | Worker 进程内编译 | 取消正在进行的 clang 编译 | 编译过程中轮询 `*stop` 标志，外部设置为 true 使编译提前退出 |
| eventide Peer cancellation | 主进程 ↔ Worker IPC | 取消已发出的 RPC 请求 | Peer 框架提供的请求级取消，JSON-RPC cancel notification |

**三层级联关系**：

```
用户编辑文档 → 主进程 update()
    │
    ├─ 1. cancellation_source.cancel()
    │     → CompileGraph 级联取消所有关联编译任务
    │
    ├─ 2. 主进程向 Worker 发送 Peer cancel notification
    │     → 取消已发出的 RPC 请求
    │
    └─ 3. Worker 收到 cancel 后设置 stop = true
          → clang 编译轮询 *stop 标志，提前退出
```

三层取消形成级联链：CompileGraph（`cancellation_source`）→ Peer cancel（IPC 层）→ `atomic_bool stop`（clang 编译层）。

---

## 8. 编译系统设计

这是整个系统最核心的部分。设计参考 eventide `examples/build_system` 的 `CompileGraph` 模式，使用 `cancellation_source`/`cancellation_token` + `event` + `with_token()` 实现可取消的编译依赖图。CompileGraph 本身采用 pull-based 模式按需编译 PCM/PCH 依赖；而对用户正在编辑的文件，diagnostics 采用 priority-aware lazy push 模式主动推送（参见 3.3 节）。

### 8.1 核心需求

1. **按需编译**：PCM/PCH 依赖 pull-based 按需编译；主文件 diagnostics 由主进程 push 驱动
2. **依赖自动解析**：编译某文件时自动递归编译其 PCM/PCH 依赖
3. **编译去重**：若 B 和 C 同时请求编译 A，A 只编译一次，后到的请求通过 `event::wait()` 等待
4. **最大并行度**：互不依赖的编译单元并行编译
5. **支持取消**：每个编译单元拥有独立的 `cancellation_source`，通过 `with_token()` 将取消传播到子任务
6. **级联失效**：文件更新时取消正在进行的编译，并递归标记所有依赖者为 dirty
7. **循环依赖检测**：对 C++20 modules 的循环 import 及时报错

### 8.2 文件分类与依赖模型

编译系统中有两类文件，依赖结构不同：

#### Module 文件（C++20 Named Modules）

Module 文件可能 import 其他模块，形成模块依赖图。编译一个 module 文件需要先编译其所有依赖的 PCM：

```
module_a.cppm  ──import──→  module_b.cppm  ──import──→  module_c.cppm
     │                            │                           │
     ▼                            ▼                           ▼
 module_a.pcm              module_b.pcm                module_c.pcm
```

依赖扫描：编译前需先扫描文件的 `import` 声明，获取模块名，再通过全局的 模块名→文件路径 映射找到依赖的 PCM 编译单元。

#### 非 Module 文件（普通 C++ 源文件）

非 module 文件没有 PCM 依赖，唯一的前置依赖是 PCH（预编译头）。情况简单很多。

#### 依赖总结

根据编译场景，前置依赖组合如下：

| 场景 | 文件类型 | 前置依赖 | 说明 |
|------|---------|---------|------|
| 打开的文件（Stateful 编译） | Module | PCM + PCH（可并行） | 编译所有依赖 PCM，同时编译 PCH |
| 打开的文件（Stateful 编译） | 非 Module | PCH | 只需编译 PCH |
| 后台索引（Index） | Module | PCM | 编译所有依赖 PCM，不需要 PCH |
| 后台索引（Index） | 非 Module | 无 | 直接编译，不需要 PCH |

注意：Module 文件的 PCM 和 PCH 是**互相独立的**（PCH 不依赖 PCM，PCM 不依赖 PCH），因此可以并行编译（参见 8.5 场景 1）。对于打开的文件，两者都需要：PCM 用于模块接口，PCH 用于加速后续增量编译。索引场景不需要 PCH，因为索引是一次性的。

### 8.3 编译单元状态

CompileGraph 中包含**两种节点**：

- **依赖产物节点**（PCM、PCH）：由 CompileGraph 通过 StatelessWorker 实际编译，拥有完整的编译生命周期（dirty → compiling → clean）。
- **源聚合节点**（主文件，如 `app.cpp`）：仅用于记录依赖边（该主文件依赖哪些 PCM/PCH），CompileGraph **不编译**这些节点——它只调用 `compile_deps()` 确保其依赖就绪，主文件本身由 `run_build_drain` 通过 StatefulWorker 编译。

每个节点在主进程的 CompileGraph 中维护如下状态：

```cpp
struct CompileUnit {
    std::string path;                       // 文件路径
    std::vector<std::string> dependencies;  // 前置依赖路径
    std::vector<std::string> dependents;    // 反向依赖 (谁依赖我)

    bool dirty = true;
    bool compiling = false;

    // 每个编译单元独立的取消源。
    // update() 时调用 source->cancel() 取消正在进行的编译，
    // 然后创建新的 source 供下次编译使用。
    std::unique_ptr<et::cancellation_source> source =
        std::make_unique<et::cancellation_source>();

    // 编译完成信号。当 compiling == true 时，其他等待者
    // co_await completion->wait() 等待完成，而非启动重复编译。
    // 每次开始编译时创建新的 event。
    std::unique_ptr<et::event> completion;
};
```

**状态转换**：

```
                    update()
  ┌───────────────────────────────────┐
  │  cancel source, new source       │
  │  dirty = true                    │
  ▼                                  │
[Dirty] ──compile()──→ [Compiling] ──完成──→ [Clean]
  ▲                         │                  │
  │                    update() 到达:          │
  │                    cancel + dirty          │
  └────────────────────────────────────────────┘
                    update()
```

### 8.4 编译图核心实现

```cpp
class CompileGraph {
    std::map<std::string, CompileUnit> units;

public:
    // === 注册编译单元 ===
    // 源聚合节点是"虚拟"节点：仅记录依赖边，dispatch_compile() 永远不会
    // 在源聚合节点上被调用。compile(path) 只调用 compile_deps() 确保其
    // PCM/PCH 依赖就绪，主文件本身由 StatefulWorker 编译。
    void register_unit(const std::string& path,
                       const std::vector<std::string>& deps) {
        auto& unit = units[path];
        unit.path = path;
        unit.dependencies = deps;
    }

    // === 编译入口：确保 path 的所有 PCM/PCH 依赖就绪 ===
    // 注意：不编译 path 本身，主文件由 StatefulWorker 编译
    et::task<bool> compile(const std::string& path, et::event_loop& loop) {
        // 递归编译 path 的所有依赖 (PCM/PCH)
        // 编译依赖本身用 compile_impl
        co_return co_await compile_deps(path, loop);
    }

    // === 文件更新 (级联失效) ===
    // 使用迭代 BFS 避免深度依赖链的栈溢出风险
    void update(const std::string& path) {
        std::vector<std::string> queue;
        queue.push_back(path);

        while (!queue.empty()) {
            auto current = std::move(queue.back());
            queue.pop_back();

            auto it = units.find(current);
            if (it == units.end()) continue;

            auto& unit = it->second;
            // 若已经 dirty，跳过（避免重复处理和循环依赖无限遍历）
            if (unit.dirty) continue;

            unit.source->cancel();
            unit.source = std::make_unique<et::cancellation_source>();
            unit.dirty = true;

            for (auto& dep : unit.dependents) {
                queue.push_back(dep);
            }
        }
    }

private:
    // compile_deps: 递归编译 path 的所有 PCM/PCH 依赖
    et::task<bool> compile_deps(const std::string& path, et::event_loop& loop) {
        auto it = units.find(path);
        if (it == units.end()) co_return true; // 无依赖信息，视为就绪

        auto& unit = it->second;

        // 对每个依赖，调用 compile_impl 编译
        for (auto& dep_path : unit.dependencies) {
            auto& dep = units[dep_path];
            if (dep.dirty && !dep.compiling) {
                loop.schedule(compile_impl(dep_path, loop));
            }
        }

        // 等待所有依赖完成，绑定源聚合节点的取消令牌
        // 这样当 update() 取消源节点时，compile_deps 能快速退出
        for (auto& dep_path : unit.dependencies) {
            auto& dep = units[dep_path];
            if (dep.compiling) {
                auto wait_result = co_await et::with_token(
                    unit.source->token(),
                    dep.completion->wait()
                );
                if (!wait_result.has_value()) {
                    co_return false; // cancelled
                }
            }
            if (dep.dirty) {
                co_return false; // 依赖编译失败
            }
        }

        co_return true; // 所有依赖就绪
    }

    // compile_impl: 编译单个依赖产物 (PCM/PCH)
    //
    // 注意：每次 co_await 恢复后必须重新 units.find(path) 获取引用，
    // 因为挂起期间 units map 可能因插入新节点而导致迭代器/引用失效
    // (虽然 std::map 保证引用稳定，但作为防御性编程的最佳实践仍然这样做)。
    et::task<bool> compile_impl(const std::string& path, et::event_loop& loop) {
        auto it = units.find(path);
        if (it == units.end()) co_return false;

        auto& unit = it->second;

        // 1) 已编译且未 dirty → 直接返回
        if (!unit.dirty) {
            co_return true;
        }

        // 2) 正在编译 → 等待完成 (编译去重)
        if (unit.compiling) {
            co_await unit.completion->wait();
            // co_await 恢复后重新查找
            auto& u = units.find(path)->second;
            co_return !u.dirty;
        }

        // 3) 开始编译
        unit.compiling = true;
        unit.completion = std::make_unique<et::event>();

        // 4) 最大并行度编译所有前置依赖
        for (auto& dep_path : unit.dependencies) {
            auto& dep = units[dep_path];
            if (dep.dirty && !dep.compiling) {
                loop.schedule(compile_impl(dep_path, loop));
            }
        }

        // 等待所有依赖完成，每个等待都绑定当前单元的取消令牌
        // co_await 恢复后重新查找引用
        for (auto& dep_path : units.find(path)->second.dependencies) {
            auto& dep = units[dep_path];
            if (dep.compiling) {
                auto& cur = units.find(path)->second;
                auto wait_result = co_await et::with_token(
                    cur.source->token(),
                    dep.completion->wait()
                );
                if (!wait_result.has_value()) {
                    auto& u = units.find(path)->second;
                    u.compiling = false;
                    u.completion->set();
                    co_await et::cancel();
                }
            }
            // co_await 恢复后重新检查依赖状态
            if (units[dep_path].dirty) {
                auto& u = units.find(path)->second;
                u.compiling = false;
                u.completion->set();
                co_await et::cancel();
            }
        }

        // 5) 所有依赖就绪，发送 BuildPCH/BuildPCM 任务到 StatelessWorker
        //    注意：这里只编译 PCM/PCH 依赖产物，不编译主文件
        {
            auto& cur = units.find(path)->second;
            auto result = co_await et::with_token(
                cur.source->token(),
                dispatch_compile(path, loop)
            );

            if (!result.has_value()) {
                auto& u = units.find(path)->second;
                u.compiling = false;
                u.completion->set();
                co_await et::cancel();
            }
        }

        // 6) 编译成功，co_await 恢复后重新查找
        auto& final_unit = units.find(path)->second;
        final_unit.dirty = false;
        final_unit.compiling = false;
        final_unit.completion->set();
        co_return true;
    }
};
```

### 8.5 依赖构建示例

#### 场景 1：打开一个 Module 文件

用户打开 `app.cpp`，它 `import math;` 且 `math` 又 `import base;`。

```
compile("app.cpp") 被触发
    │
    ├─ 扫描 app.cpp 的 import → 发现依赖 math.pcm
    ├─ 构建依赖: app.cpp.dependencies = ["math.pcm", "app.pch"]
    │
    ├─ 步骤 4: 并行启动所有依赖编译
    │     ├─ loop.schedule(compile_impl("math.pcm"))
    │     │     │
    │     │     ├─ 扫描 math.cppm 的 import → 依赖 base.pcm
    │     │     ├─ loop.schedule(compile_impl("base.pcm"))
    │     │     │     └─ base.pcm 无依赖 → 直接编译 → dirty=false
    │     │     ├─ co_await base.pcm.completion->wait()
    │     │     └─ base.pcm ready → 编译 math.pcm → dirty=false
    │     │
    │     └─ loop.schedule(compile_impl("app.pch"))
    │           └─ PCH 无 PCM 依赖 → 直接编译 → dirty=false
    │           (注: math.pcm 和 app.pch 并行编译)
    │
    ├─ co_await math.pcm.completion->wait()  ✓
    ├─ co_await app.pch.completion->wait()   ✓
    │
    └─ 所有依赖就绪 → 编译 app.cpp (发送到 Worker)
```

#### 场景 2：后台索引一个 Module 文件

与场景 1 类似，但 dependencies 中不包含 PCH：

```
index("app.cpp").dependencies = ["math.pcm"]  // 无 PCH
```

#### 场景 3：打开一个普通文件

```
compile("main.cpp") 被触发
    │
    ├─ 非 module 文件，无 PCM 依赖
    ├─ dependencies = ["main.pch"]
    │
    ├─ loop.schedule(compile_impl("main.pch"))
    ├─ co_await main.pch.completion->wait()
    │
    └─ PCH ready → 编译 main.cpp
```

#### 场景 4：后台索引一个普通文件

```
index("main.cpp").dependencies = []  // 无任何前置依赖
    │
    └─ 直接编译并索引
```

### 8.6 取消传播

每个 `CompileUnit` 拥有独立的 `cancellation_source`。

```
用户编辑 base.cppm
  → update("base.cppm")
      → base.pcm.source->cancel()         // 取消 base.pcm 编译
      → base.pcm.dirty = true
      → 递归: update("math.pcm")          // math.pcm 依赖 base.pcm
          → math.pcm.source->cancel()
          → math.pcm.dirty = true
          → 递归: update("app.cpp")        // app.cpp 依赖 math.pcm
              → app.cpp.source->cancel()
              → app.cpp.dirty = true
```

`with_token(token, task)` 的语义：
1. 向 `cancellation_state` 注册子任务的 `async_node`
2. 当 `source.cancel()` 被调用时，所有注册的 node 被取消
3. 子任务取消后返回 `std::expected<T, cancellation>` 无值状态
4. 调用者检查 `result.has_value()` 判断是否被取消

### 8.7 循环依赖检测

在 `compile_impl` 递归链中通过栈检测循环：

```cpp
et::task<bool> compile_impl(const std::string& path, et::event_loop& loop,
                             std::vector<std::string>& stack) {
    if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
        // 循环依赖: A → B → C → A，报告诊断错误
        co_return false;
    }
    stack.push_back(path);
    // ... 正常编译逻辑 ...
    stack.pop_back();
    co_return true;
}
```

### 8.8 编译图与主进程请求流的整合

编译图运行在主进程中。主进程驱动全流程：先通过 CompileGraph 确保 PCM/PCH 依赖就绪，再发送 `clice/worker/compile` 给 StatefulWorker 编译主文件。

**Build Drain 整合 CompileGraph**：

```
run_build_drain(uri) — 主进程协程:
    │
    ├─ 查询文件的编译命令 (CDB)
    ├─ 扫描文件的 import 依赖 (通过 FuzzyGraph)
    ├─ compile_graph.register_unit(path, dependencies)
    │
    ├─ co_await compile_graph.compile(path)
    │     └─ 递归编译: X 的 PCM 依赖 → X 的 PCH
    │     └─ 不编译 X 本身（X 由 StatefulWorker 编译）
    │
    ├─ 依赖就绪后, 发送 worker::CompileParams 到 StatefulWorker
    │     └─ 携带 directory, arguments, pch, pcms
    │     └─ StatefulWorker 使用已就绪的 PCM/PCH 编译主文件
    │     └─ 返回 diagnostics
    │
    └─ 发布 diagnostics 给 LSP client
```

**Hover 等 Feature Request 的流程**：

```
Hover 请求到达 (文件 X)
    │
    ├─ 路由到 StatefulWorkerPool (assign_worker)
    ├─ 发送 worker::HoverParams (仅 uri + position)
    │     └─ StatefulWorker 已持有 AST（由之前的 compile 请求构建）
    │     └─ 遍历 AST 返回 hover 信息
    │
    └─ 返回结果给 LSP client
```

**Completion/SignatureHelp 请求的流程**：

这两种请求发往 StatelessWorker，但同样需要 PCH/PCM 就绪。主进程在发送请求前**必须先通过 CompileGraph 确保依赖编译完成**：

```
on_completion(params) / on_signature_help(params)
    │
    ├─ co_await compile_graph.compile(path)  ← 确保 PCM/PCH 就绪
    │
    ├─ **Preamble 变更检测**:
    │     计算当前文本 preamble 区域的 hash (compute_preamble_bound → 取前缀 hash)
    │     比较与当前 PCH 构建时的 preamble hash
    │     若不一致 (用户添加/删除了 #include):
    │       触发 PCH 重建 → co_await compile_graph.compile(pch_path)
    │       （这确保 Completion 使用最新的 #include）
    │
    ├─ 构造 worker::CompletionParams / worker::SignatureHelpParams
    │     └─ 携带 pch, pcms（从 CompileGraph 获取已编译的产物路径）
    └─ co_await stateless_workers.completion(params) / .signature_help(params)
```

所有使用 PCH/PCM 的请求路径（Build Drain、Completion、SignatureHelp）都必须先经过 CompileGraph。Completion/SignatureHelp 额外检测 preamble 变更，确保用户新增的 `#include` 能被正确处理。

编译期间若用户编辑了文件：
1. `update()` 被调用 → 级联取消正在进行的编译
2. `compile()` 的 `catch_cancel()` 捕获取消
3. 新的编辑触发新的 build drain → 主进程重新协调编译

### 8.9 编译数据库 (CDB)

```cpp
class CompilationDatabase {
    // 加载 compile_commands.json
    // 增量更新: UpdateInfo { Unchanged, Inserted, Deleted }
    // 查询: lookup(filename) → CompileCommand
    //   - ignore_unknown: 未知文件是否返回默认命令
    //   - resource_dir: clang 资源目录
    //   - query_toolchain: 是否查询工具链信息
};
```

支持的工具链：GCC、Clang、MSVC、ClangCL、NVCC、Intel、Zig。

CDB 加载时按配置剔除不需要的编译参数。

### 8.10 缓存管理

缓存目录结构：

```
<cache_dir>/
├── cache.json        # PCH/PCM 元信息 (编译参数、源文件、SHA256、编译时间)
├── *.pch             # 预编译头文件
├── *.pcm             # 预编译模块文件
└── graph.idx         # 全局包含图缓存
```

新鲜度判断：
1. 先检查 mtime，若未变则认为有效
2. 若 mtime 变了，计算 SHA256 比较
3. 若 SHA256 一致，仅更新 mtime 记录，跳过重新编译

**缓存容量管理**：

缓存目录有最大容量限制（可配置，默认 10 GB），淘汰策略为 LRU by last-access-time：

- 每次启动时扫描缓存目录，记录每个 `.pch`/`.pcm` 的大小和最后使用时间
- 当总大小超过限制时，淘汰最久未使用的文件
- 索引缓存（`graph.idx`、`project.idx`、`shards/*.idx`）不参与容量限制（通常较小）
- CDB 变更导致的编译参数变化会使旧 PCH/PCM 失效，失效文件在下次扫描时删除

---

## 9. 全局包含图设计

### 9.1 双图模型

系统中存在两种本质不同的包含图，**不合并存储**，但在上层提供统一查询接口。

#### 模糊图 (Fuzzy Graph) — 以文件为中心

模糊图通过 `scan_fuzzy()` 构建。该函数利用 clang 的 `DependencyDirectivesGetter` 机制，在预处理前对每个文件调用 `scanSourceForDependencyDirectives()` 获取依赖指令列表，然后**过滤掉所有 `#define`/`#undef` 和条件指令**（`#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif`），使预处理器无条件地处理所有 `#include`。这意味着：

- 同一文件在同一编译命令下，扫描结果是**上下文无关的**，可以全局缓存复用
- 标准库/系统头文件只需扫描一次，后续直接跳过，速度相比 clang 完整预处理提升约 **100 倍**
- 扫描数千个文件可在数秒内完成
- `scan_fuzzy()` 返回 `StringMap<ScanResult>`，递归扫描主文件及其所有传递包含的头文件
- 每个 `IncludeInfo` 携带 `conditional` 标记（在过滤前从原始指令结构中预计算）和 `not_found` 标记
- 跨文件共享 `SharedScanCache`，同一头文件只需扫描一次

**代价**：剥离宏和条件指令后会丢失部分信息：
- 宏展开后的 `#include`（如 `#include PLATFORM_HEADER`）会找不到，导致**多找不少找**（所有 `#ifdef` 分支都被扫描）
- 找不到的头文件通过 `FileNotFound` 回调标记为 `not_found`，扫描继续不中断
- 不同编译命令的 `-I` 路径不同，同一 `#include` 可能解析到不同文件。通过**上下文等价类**（`-I` 参数相同的编译命令归为同一 ContextID）来管理

**数据结构**：

```cpp
// 简化概念模型（忽略 ContextID）
// 实际存储结构见 9.4 节
struct FuzzyGraph {
    Map<FileID, Set<FileID>> forward;   // 概念: 文件 A include 了哪些
    Map<FileID, Set<FileID>> backward;  // 概念: 文件 B 被哪些 include
};
```

**增量更新**（基于 Delta）：

```
文件 A 修改 → 重新快速扫描 A → 得到 NewIncludes
取出旧集合 OldIncludes = forward[A]
计算差异:
  Added   = NewIncludes - OldIncludes
  Removed = OldIncludes - NewIncludes
更新正向图: forward[A] = NewIncludes
更新反向图 (仅 Delta):
  for f in Added:   backward[f].insert(A)
  for f in Removed: backward[f].remove(A)
```

Delta 更新极其轻量，仅影响实际变更的 `#include` 指令。

#### 精确图 (Exact Graph) — 以 TU 为中心

精确图由完整编译过程产生，包含宏展开后的准确包含关系。它是一棵**包含树 (Include Tree)**，挂载在每个 TU 的索引中。

- 存储在 `TUIndex.graph` 中
- 头文件记录被哪个源文件 include，以及 include 的位置
- 脱离具体 TU 讨论精确图没有意义（宏是上下文相关的）
- 更新方式：TU 的 AST 重建完成后，整体替换旧的 Include Tree，不做节点 diff

**两种图的结构性差异**：

| 维度 | 模糊图 | 精确图 |
|------|--------|--------|
| 中心 | 文件 | TU (编译单元) |
| 拓扑 | DAG (有向无环图) | Tree (包含树/森林) |
| 上下文 | 无宏上下文，仅编译命令搜索路径 | 完整宏上下文 |
| 构建速度 | 极快（毫秒到秒级） | 缓慢（需完整编译） |
| 准确性 | 近似（多找不少找） | 精确 |
| 更新粒度 | 文件级 Delta | TU 级整体替换 |
| 存储 | `graph.idx` | `TUIndex.graph` |

### 9.2 统一查询接口 (IncludeQueryService)

上层功能模块不直接操作两种图，通过统一接口查询，内部实现优先级降级策略：

```cpp
class IncludeQueryService {
    FuzzyGraph& fuzzy;
    ProjectIndex& precise;  // 精确图存储在各 TU 索引中

public:
    // 查找哪些源文件包含了指定头文件 (用于 Header Context)
    // 优先查精确图，降级到模糊图
    std::vector<FileID> find_host_sources(FileID header);

    // 查找文件变更影响的所有文件 (用于失效传播)
    // 模糊图快速返回近似结果，精确图标记 dirty TU
    std::vector<FileID> get_affected_files(FileID changed);

    // 查找文件的所有 include (正向查询)
    std::vector<FileID> get_includes(FileID file);
};
```

**查询策略**：

```
find_host_sources(B.h):
  1. 查询精确图: 遍历所有 TU 的 Include Tree，
     看哪些 TU 包含了 B.h → 精确结果
  2. 若精确图未覆盖或后台索引未完成:
     降级到模糊图: 沿 backward 反向图向上找到 .cpp 文件
     → 近似结果，但对于寻找 Host 足够
```

### 9.3 启动时扫描策略

```
1. 尝试加载 graph.idx (缓存)
2. 若不存在:
   a. 遍历 CDB 中的所有源文件
   b. 对每个源文件调用 scan_fuzzy()，共享 SharedScanCache
      - 内部通过 DependencyDirectivesGetter 递归扫描所有传递包含的头文件
      - SharedScanCache 确保同一头文件只扫描一次
      - 条件指令和 #define 被过滤，所有 #include 分支无条件处理
   c. 同时通过 scan() 词法扫描 module 声明 (模块名 ↔ 文件路径映射)
   d. 不扫描模块依赖关系 (开销高，lazy 使用 scan_precise() 处理)
   e. 构建模糊包含图 (正向 + 反向)
   f. 保存为 graph.idx
3. 整个过程预期在数秒内完成
```

### 9.4 多编译上下文处理

同一文件在不同编译命令下可能有不同的 include 解析结果。处理方式：

**上下文等价类 (Context Equivalence Class)**：将影响 include 解析的编译参数（`-I`, `-isystem` 等）相同的编译命令归为同一个 ContextID。典型项目中即使有数千个源文件，ContextID 通常也只有两三种。

```
CDB 中 1000 个源文件 → 可能只有 3 种 ContextID:
  Context 0: 主项目 (src/*.cpp)
  Context 1: 测试代码 (test/*.cpp)
  Context 2: 第三方代码 (vendor/*.cpp)
```

模糊图中，同一文件在不同 ContextID 下可能有不同的 include 集合。存储方式：

```cpp
// 正向图: (FileID, ContextID) → Set<FileID>
Map<pair<FileID, ContextID>, Set<FileID>> forward;
// 反向图: FileID → Set<(FileID, ContextID)>
Map<FileID, Set<pair<FileID, ContextID>>> backward;
```

---

## 10. Header Context 设计

非自包含头文件（如 `.inc` 文件、没有 include guard 的内部头文件）无法独立编译，需要找到一个包含它的源文件作为编译上下文。

### 10.1 核心思路

对于给定的非自包含头文件，通过包含图找到一个包含它的源文件（Host），然后：

1. 预处理该源文件，找到目标头文件被 `#include` 的位置
2. 递归展平该头文件之前的所有 include 链条，生成一个虚拟的 preamble 文件
3. 在编译目标头文件时，通过 `-include` 隐式包含该 preamble 文件
4. 使用 `#line` 指令修正文件名和行号信息，确保诊断信息准确

```
Host 源文件 main.cpp:
  #include <vector>         ─┐
  #include "config.h"        │  展平为 preamble.h
  #include "utils.h"        ─┘
  #include "target.h"  ← 目标头文件

编译 target.h 时:
  clang -include preamble.h target.h
  (preamble.h 中包含了 target.h 之前所有内容的展平文本)
```

### 10.2 Preamble 生成

Preamble 的生成**不执行预处理，也不展开所有头文件**，而是按需最小化、语义不变地补充目标头文件前面必要的代码。

核心原则：沿着包含图从源文件出发，找到目标头文件被 include 的路径。路径上每一层只需原样 append 该层中目标 include 位置之前的代码（包含其中的 `#include` 指令，不展开它们）。只有当目标头文件不是被源文件直接 include，而是被某个中间头文件 include 时，才需要递归地"展开"那个中间头文件的前缀部分。

**示例 1：直接 include**

```
Host 源文件 main.cpp:
  #include <vector>
  #include "config.h"
  int global_var = 42;
  #include "target.h"  ← 目标头文件

preamble.h 内容 (原样拷贝 target.h 之前的代码):
  #line 1 "main.cpp"
  #include <vector>
  #include "config.h"
  int global_var = 42;
```

这里 `<vector>` 和 `"config.h"` 不需要展开，保持 `#include` 指令原样即可。编译器会自行处理它们。

**示例 2：间接 include（需要递归展开中间层）**

```
Host 源文件 main.cpp:
  #include <vector>
  #include "framework.h"   ← framework.h 内部 include 了 target.h
  #include "other.h"

framework.h:
  #include "types.h"
  void setup();
  #include "target.h"  ← 目标头文件在这里

preamble.h 内容:
  #line 1 "main.cpp"
  #include <vector>         // main.cpp 中 framework.h 之前的代码 (原样)
  #line 1 "framework.h"
  #include "types.h"        // framework.h 中 target.h 之前的代码 (原样)
  void setup();
```

只有 `framework.h` 这个中间层需要被"打开"并取出 `target.h` 之前的部分。`<vector>`、`"types.h"` 等仍保持 `#include` 指令形式，不展开。

**生成算法（含 suffix 支持）**：

除了 preamble（include 前的文本），还需要生成 suffix（include 后的文本）。这是因为目标文件可能在 namespace 内被 include（如 `.inc` 文件），include 后面的 `}` 闭合大括号等代码对于语义正确性是必要的。

```
generate_preamble_and_suffix(source, target):
    preamble = ""
    suffix = ""
    current_file = source
    while current_file != target:
        找到 current_file 中 include 路径上下一层文件的位置 pos
        preamble += "#line 1 \"" + current_file + "\"\n"
        preamble += current_file 中 pos 之前的所有文本

        // 收集 include 之后的文本作为 suffix（逆序拼接）
        after_text = current_file 中 pos 之后到文件末尾的文本
        if after_text 非空:
            suffix = "#line " + (pos+1) + " \"" + current_file + "\"\n"
                   + after_text + "\n" + suffix

        current_file = 路径上的下一层文件
    return (preamble, suffix)
```

编译时：`clang -include preamble.h target.inc -include suffix.h`
或将三者拼接为单一虚拟文件：`preamble + target内容 + suffix`

**示例 3：namespace 内 include（.inc 文件）**

```
Host 源文件 main.cpp:
  #include <vector>
  namespace detail {
  #include "ops.inc"  ← 目标 .inc 文件
  } // namespace detail
  int main() { ... }

preamble.h 内容:
  #line 1 "main.cpp"
  #include <vector>
  namespace detail {

suffix.h 内容:
  #line 4 "main.cpp"
  } // namespace detail
  int main() { ... }
```

这样 `ops.inc` 被编译时拥有正确的 namespace 上下文和闭合大括号。

每层只取 include 位置前/后的文本原样 append，不做预处理、不展开宏、不递归展开其他 `#include`。生成的 preamble + suffix 是语义等价的完整上下文。

**`#line` 指令的作用**：由于多层文件的文本被拼接到一个虚拟文件中，`__FILE__` 和 `__LINE__` 等宏的语义会改变。通过 `#line` 指令在每层切换时恢复正确的文件名和行号，确保编译器诊断信息指向正确位置。

**与 PCH 的兼容性**：这种方式与 PCH 完全不冲突。preamble 文件本身可以被编译为 PCH 缓存，目标头文件仍然有独立的 PCH 缓存。额外开销仅有一次包含图路径查找 + 文本拼接，非常轻量。

### 10.3 Host 源文件的选择策略

通过包含图查找 Host 源文件，优先级如下：

```
1. 优先尝试自包含编译:
   直接以自包含方式编译目标头文件。
   如果编译产生的 Error 数量低于阈值 → 视为自包含，无需 Host。
   (绝大部分头文件是自包含的)

2. 查询精确图:
   若后台索引已覆盖，从精确图中找到包含该头文件的 TU。
   精确结果，连宏状态都匹配。

3. 查询模糊图:
   沿反向图向上查找 .cpp 文件。
   启发式选择最近的（同目录 > 同 module > 父目录）。

4. 用户手动选择:
   通过扩展 LSP 命令 (workspace/executeCommand)，
   在编辑器状态栏显示当前 Host，用户可点击切换。
```

### 10.4 注意事项

- **`#pragma once` / Include Guards**：展平文本后需正确处理头文件保护符。如果 preamble 中已展开 `A.h`，而目标头文件内部又 `#include "A.h"`，预处理器需能正确识别已包含
- **Namespace 作用域**：如果非自包含文件是在 namespace 内被 include（常见于 `.inc` 文件），展平时需要保留外层的 namespace 声明和闭合大括号
- **Preamble 大小**：如果头文件在 include 链条很深处，preamble 可能很大。但由于可以缓存为 PCH，首次开销大但后续增量编译很快

---

## 11. 索引系统设计

### 11.1 索引架构总览

索引系统采用三层结构，数据从 Worker 产出流向主进程全局索引：

```
StatelessWorker                     MasterServer (主进程)
┌────────────────┐       IPC       ┌──────────────────────────────────────┐
│                │   (FlatBuffers  │                                      │
│  编译 + 遍历   │   as opaque    │  ProjectIndex (全局符号表)            │
│      ↓         │    bytes)      │  ├── path_pool: 路径去重池           │
│  TUIndex 产出  │ ─────────────→ │  ├── symbols: SymbolHash → Symbol    │
│                │                │  └── indices: path_id → tu_id        │
└────────────────┘                │                                      │
                                  │  MergedIndex (分片索引, 每文件一个)   │
                                  │  ├── canonical_id 去重               │
                                  │  ├── occurrences: Occurrence → Bitmap│
                                  │  ├── relations: Symbol → Relation →  │
                                  │  │               Bitmap              │
                                  │  └── header/compilation contexts     │
                                  │                                      │
                                  │  查询流程:                            │
                                  │  cursor → MergedIndex.lookup(offset) │
                                  │  → SymbolHash                        │
                                  │  → ProjectIndex.symbols[hash]        │
                                  │    .reference_files (Bitmap)          │
                                  │  → 线程池并行查询各分片               │
                                  └──────────────────────────────────────┘
```

**数据流**：Worker 编译源文件 → 遍历 AST 产出 TUIndex → 序列化为 FlatBuffers → 通过 bincode IPC 传输到主进程 → 主进程合并到 ProjectIndex（全局符号表）和 MergedIndex（分片索引）→ 查询时从分片索引检索。

### 11.2 索引数据结构

以下基于 `src/index/` 中的实际代码精确描述各层数据结构。

#### SymbolHash 与 SymbolID

```cpp
using SymbolHash = std::uint64_t;  // xxh3_64bits(USR)

struct SymbolID {
    std::uint64_t hash;   // SymbolHash
    std::string name;     // 符号显示名
};
```

- **USR (Unified Symbol Resolution)**：clang 的稳定标识符，跨 TU 一致
- **SymbolHash**：对 USR 字符串计算 `llvm::xxh3_64bits`，得到 64 位哈希作为符号的全局唯一标识

#### Occurrence

```cpp
struct Occurrence {
    LocalSourceRange range;   // [begin, end] 字节范围
    SymbolHash target;        // 引用的符号
};
```

记录"在某文件的 `[begin, end]` 处出现了对符号 `target` 的引用"。

#### Relation 与 RelationKind

```cpp
struct RelationKind {
    enum Kind : std::uint32_t {
        Invalid, Declaration, Definition, Reference, WeakReference,
        Read, Write, Interface, Implementation, TypeDefinition,
        Base, Derived, Constructor, Destructor, Caller, Callee
    };
};

struct Relation {
    RelationKind kind;
    std::uint32_t padding;
    LocalSourceRange range;
    SymbolHash target_symbol;
};
```

Relation 记录符号间的语义关系。对于 Declaration/Definition，`target_symbol` 字段通过 bit-cast 存储定义范围；对于 Caller/Callee 等，存储目标符号的哈希。

#### FileIndex（每文件索引）

```cpp
struct FileIndex {
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations;
    std::vector<Occurrence> occurrences;

    std::array<std::uint8_t, 32> hash();  // SHA256，用于 canonical_id 去重
};
```

一个文件内的所有符号出现和关系。`hash()` 计算内容的 SHA256，用于跨 TU 去重（参见 11.3 节）。

#### Symbol 与 SymbolTable

```cpp
struct Symbol {
    std::string name;
    SymbolKind kind;
    Bitmap reference_files;   // roaring::Roaring，记录引用该符号的文件集合
};

using SymbolTable = llvm::DenseMap<SymbolHash, Symbol>;
```

`reference_files` 是 Roaring Bitmap，高效存储引用了该符号的文件 ID 集合，用于查询时快速定位需要搜索的分片。

#### TUIndex（单 TU 索引，Worker 产出）

```cpp
struct TUIndex {
    std::chrono::milliseconds built_at;                       // 编译时间戳
    IncludeGraph graph;                                        // 该 TU 的包含树
    SymbolTable symbols;                                       // TU 内符号表
    llvm::DenseMap<clang::FileID, FileIndex> file_indices;    // 每个包含文件的索引
    FileIndex main_file_index;                                 // 主文件索引

    static TUIndex build(CompilationUnitRef unit);            // 由 AST 遍历构建
};
```

`TUIndex::build()` 在 StatelessWorker 中通过遍历 AST 产生。每个被包含的文件对应一个 `FileIndex`，主文件单独存储。

#### ProjectIndex（全局符号表）

```cpp
struct ProjectIndex {
    PathPool path_pool;                                         // 文件路径去重池
    llvm::DenseMap<std::uint32_t, std::uint32_t> indices;      // source path_id → tu_id
    SymbolTable symbols;                                        // 全局符号表

    llvm::SmallVector<std::uint32_t> merge(TUIndex& index);    // 合并 TU 索引
    void serialize(llvm::raw_ostream& os);                      // 序列化到磁盘
    static ProjectIndex from(const void* data);                 // 从 FlatBuffer 反序列化
};
```

PathPool 将文件路径映射为 `uint32_t` ID，避免重复存储路径字符串：

```cpp
struct PathPool {
    llvm::BumpPtrAllocator allocator;
    std::vector<llvm::StringRef> paths;
    llvm::DenseMap<llvm::StringRef, std::uint32_t> cache;

    auto path_id(llvm::StringRef path);       // 获取或创建 path ID
    llvm::StringRef path(std::uint32_t id);   // 通过 ID 查路径
};
```

#### MergedIndex（分片索引）

每个文件对应一个 MergedIndex，是查询的核心数据结构。内部实现（`Impl`）：

```cpp
struct MergedIndex::Impl {
    // Canonical ID 管理
    llvm::StringMap<std::uint32_t> canonical_cache;     // SHA256 → canonical_id
    std::uint32_t max_canonical_id = 0;
    std::vector<std::uint32_t> canonical_ref_counts;    // 引用计数
    roaring::Roaring removed;                            // 已删除的 canonical_id 集合

    // 核心索引数据：每个 Occurrence/Relation 关联一个 Bitmap，记录属于哪些 canonical_id
    llvm::DenseMap<Occurrence, roaring::Roaring> occurrences;
    llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> relations;

    // 上下文信息
    llvm::SmallDenseMap<std::uint32_t, HeaderContext, 2> header_contexts;
    llvm::SmallDenseMap<std::uint32_t, CompilationContext, 1> compilation_contexts;
};
```

MergedIndex 支持两种存储模式：
- **磁盘模式**：持有 `MemoryBuffer`，直接从 FlatBuffer 读取（零拷贝，只读查询）
- **内存模式**：`load_in_memory()` 将 FlatBuffer 反序列化到 `Impl`（支持修改）

只在需要合并新数据时才反序列化为 `Impl`，只读查询直接访问 FlatBuffer 数据。

**并发隔离 (Atomic Swap)**：MergedIndex 内部持有 `std::shared_ptr<llvm::MemoryBuffer>` 指向当前磁盘数据。查询线程通过 `shared_ptr` 拷贝获取当前 buffer 后直接查询（只读，天然线程安全）。Merge 操作在事件循环线程：`load_in_memory()` → 修改 `Impl` → `serialize()` 到新 buffer → `std::atomic_store` 替换 `shared_ptr`。查询线程持有的旧 `shared_ptr` 在最后一个查询结束后自动释放。这保证了 merge 不阻塞查询线程，也无需 read-write lock（参见 7.2 节）。

### 11.3 分片索引与 Canonical ID

#### 去重机制

同一个头文件被不同源文件 include 时，如果内容在语义上完全相同（宏展开后 occurrences 和 relations 一致），其 FileIndex 的 SHA256 哈希值相同。MergedIndex 利用这一点进行去重：

```
源文件 A.cpp include "shared.h" → FileIndex(shared.h) → SHA256 = 0xABCD...
源文件 B.cpp include "shared.h" → FileIndex(shared.h) → SHA256 = 0xABCD...

MergedIndex(shared.h):
  canonical_cache["0xABCD..."] = canonical_id 0
  canonical_ref_counts[0] = 2     ← 两个 TU 共享同一份数据
  occurrences 和 relations 只存一份
```

#### Canonical ID 分配流程

```cpp
auto hash = index.hash();  // SHA256 of FileIndex
auto [it, success] = canonical_cache.try_emplace(hash_key, max_canonical_id);
auto canonical_id = it->second;

if (!success) {
    // 哈希已存在 → 去重，仅增加引用计数
    canonical_ref_counts[canonical_id] += 1;
    removed.remove(canonical_id);
    return;
}

// 新哈希 → 分配新 canonical_id，写入 occurrences 和 relations
for (auto& occurrence : index.occurrences) {
    occurrences[occurrence].add(canonical_id);  // Roaring bitmap
}
for (auto& [symbol_id, rels] : index.relations) {
    for (auto& relation : rels) {
        relations[symbol_id][relation].add(canonical_id);
    }
}
canonical_ref_counts.emplace_back(1);
max_canonical_id += 1;
```

#### 引用计数与生命周期

当某个 TU 重新索引时，其旧的 canonical_id 引用计数减 1。当计数降为 0 时，该 canonical_id 被加入 `removed` 集合。查询时通过 `removed` bitmap 过滤已删除的数据。

#### Roaring Bitmap 的作用

每个 Occurrence 和 Relation 都关联一个 `roaring::Roaring` bitmap，记录哪些 canonical_id 包含该数据。这解决了两个问题：

1. **去重**：相同内容的头文件只需存储一次 Occurrence/Relation，通过 bitmap 记录多个 canonical_id
2. **删除**：删除一个 canonical_id 不需要遍历所有数据，只需更新 `removed` bitmap

### 11.4 索引构建与调度

#### 空闲检测

索引只在用户空闲时执行，避免干扰交互式操作（补全、hover 等）：

- **idle callback**：事件循环的 idle 回调触发索引调度
- **最小空闲时间阈值**：两者结合，要求用户至少空闲 N 毫秒后才开始索引，避免打字间隙频繁启停索引进程
- **让步机制**：若用户在索引过程中恢复编辑，索引任务暂停（通过 cancellation token），等下次空闲再继续

#### 索引进程优先级

索引使用 StatelessWorkerPool 中的专用后台索引 Worker：

- 主进程将索引任务分发到专门的低优先级 StatelessWorker 进程
- 该进程的 OS 调度优先级最低（`nice` / `IDLE_PRIORITY_CLASS`）
- 编译图中索引任务的优先级最低（参见 5.1 节任务优先级表）

#### 索引的前置依赖

索引不需要 PCH（无需加速增量编译），前置依赖简化：

| 场景 | 前置依赖 | 说明 |
|------|---------|------|
| 索引 Module 文件 | 依赖的 PCM | 只需要模块接口文件 |
| 索引非 Module 文件 | 无 | 直接编译并索引 |

参见 8.2 节依赖总结表。

### 11.5 索引传输（零拷贝）

#### 传输流程

```
StatelessWorker                          MasterServer
┌─────────────────────┐                 ┌─────────────────────┐
│ 1. TUIndex::build() │                 │                     │
│    遍历 AST 产出     │                 │                     │
│    TUIndex          │                 │                     │
│                     │                 │                     │
│ 2. FlatBufferBuilder│  bincode IPC    │ 4. 直接读取         │
│    序列化 TUIndex   │  ────────────→  │    FlatBuffers 数据  │
│    为 FlatBuffers   │  opaque bytes   │    (零拷贝)         │
│                     │                 │                     │
│ 3. 作为 opaque      │                 │ 5. 合并到           │
│    bytes 放入       │                 │    ProjectIndex &   │
│    bincode 消息     │                 │    MergedIndex      │
└─────────────────────┘                 └─────────────────────┘
```

#### 关键设计

- Worker 将 TUIndex 序列化为 FlatBuffers 二进制数据
- FlatBuffers 数据作为 **opaque bytes** 嵌入 bincode IPC 消息传输，不需要在 Worker 端反序列化再以 bincode 重新序列化
- 主进程收到后直接读取 FlatBuffers 数据，FlatBuffers 的设计天然支持零拷贝访问（无需反序列化即可读取字段）
- 对于磁盘持久化的 MergedIndex，可以 mmap 文件后直接查询，同样是零拷贝

### 11.6 索引合并

主进程收到 Worker 返回的 TUIndex 后，执行以下合并流程：

#### 步骤 1：路径映射

将 TUIndex 中的文件路径（`IncludeGraph.paths`）映射到 ProjectIndex 的 PathPool，获得全局唯一的 `path_id`。

#### 步骤 2：合并符号到 ProjectIndex

```
对 TUIndex.symbols 中的每个 (SymbolHash, Symbol):
  若 ProjectIndex.symbols 中已有该 hash:
    合并 reference_files bitmap (Roaring OR 操作)
    更新 name 和 kind (若有更完整的信息)
  否则:
    插入新符号
```

`Symbol.reference_files` bitmap 合并后，全局符号表知道哪些文件引用了该符号。

#### 步骤 3：合并 FileIndex 到 MergedIndex

对 TUIndex 中的每个 `(FileID, FileIndex)` 对：

1. 通过 IncludeGraph 获取文件路径 → path_id → 找到对应的 MergedIndex
2. 对 MergedIndex 调用 `merge()`，基于 canonical_id 去重（参见 11.3 节）
3. 若 FileIndex 的 SHA256 首次出现，将其 occurrences 和 relations 写入 MergedIndex
4. 若 SHA256 已存在，仅增加引用计数

#### 步骤 4：更新上下文

对主文件（源文件）创建 CompilationContext：

```cpp
struct CompilationContext {
    std::uint32_t version;
    std::uint32_t canonical_id;
    std::uint64_t build_at;                          // 编译时间戳
    std::vector<IncludeLocation> include_locations;  // 包含的头文件列表
};
```

对被包含的头文件创建 HeaderContext：

```cpp
struct HeaderContext {
    std::uint32_t version;
    llvm::SmallVector<IncludeContext> includes;  // (include_id, canonical_id) 对
};
```

CompilationContext 用于 `need_update()` 判断是否需要重新索引（比较时间戳和依赖关系）。

### 11.7 索引查询

#### Find References 流程

```
1. 定位文件分片
   cursor (文件 F, 字节偏移 offset)
   → 找到 F 对应的 MergedIndex

2. 二分查找符号
   MergedIndex.lookup(offset, callback)
   → occurrences 按 range.end 排序
   → std::lower_bound 找到 range.contains(offset) 的 Occurrence
   → 取得 SymbolHash

3. 查询引用该符号的文件集合
   ProjectIndex.symbols[hash].reference_files
   → Roaring Bitmap，包含所有引用该符号的文件 path_id

4. 线程池并行查询
   对 reference_files 中的每个 path_id:
     找到对应的 MergedIndex
     MergedIndex.lookup(symbol, RelationKind::Reference, callback)
     → 收集所有 Relation 结果

5. 合并结果
   各线程返回的 Relation 列表合并
   → 转换为 LSP Location 返回给 client
```

#### Go To Definition 流程

与 Find References 类似，但使用 `RelationKind::Definition` 过滤：

```
cursor → MergedIndex.lookup(offset) → SymbolHash
→ ProjectIndex 查引用文件
→ 并行查询 MergedIndex.lookup(symbol, RelationKind::Definition)
→ 返回 Definition 位置
```

对于 Declaration/Definition 类型的 Relation，定义范围存储在 `target_symbol` 字段中（通过 bit-cast），可以直接获取定义的精确范围。

#### Rename 流程

Rename 本质是 Find References 的超集——找到符号的所有出现位置，转换为 TextEdit：

```
1. cursor → MergedIndex.lookup(offset) → SymbolHash

2. ProjectIndex.symbols[hash].reference_files → 所有引用文件

3. 线程池并行查询各分片:
   MergedIndex.lookup(symbol, RelationKind::Reference | Declaration | Definition, callback)
   → 收集所有 Relation，每个 Relation 的 range 即为需要替换的文本范围

4. 按文件分组，生成 WorkspaceEdit:
   { file_path → [TextEdit { range, newText }] }
```

与 Find References 的区别：
- 需要同时收集 Reference、Declaration、Definition 三种关系（重命名必须覆盖声明和定义）
- 结果不是 Location 列表，而是 TextEdit 列表（每个位置替换为新名字）
- 可能需要额外校验：宏展开中的引用、只读文件中的引用等，不能盲目替换

#### Call Hierarchy 流程

Call Hierarchy 使用 `Caller`/`Callee` 关系。LSP 协议分三步：`prepareCallHierarchy` → `incomingCalls` / `outgoingCalls`。

**Prepare**（定位函数符号）：

```
cursor → MergedIndex.lookup(offset) → SymbolHash
→ 确认符号是函数/方法 (检查 SymbolKind)
→ 返回 CallHierarchyItem { name, kind, uri, range }
```

**Incoming Calls**（谁调用了这个函数）：

```
1. 已知目标函数 SymbolHash

2. ProjectIndex.symbols[hash].reference_files → 引用文件集合

3. 线程池并行查询:
   MergedIndex.lookup(symbol, RelationKind::Caller, callback)
   → 每个 Relation 的 target_symbol 是调用者的 SymbolHash
   → Relation 的 range 是调用发生的位置

4. 对每个调用者 SymbolHash，查 ProjectIndex 获取其名称和定义位置
   → 组装 CallHierarchyIncomingCall { from: 调用者, fromRanges: [调用位置] }
```

**Outgoing Calls**（这个函数调用了谁）：

```
1. 已知源函数 SymbolHash

2. 找到源函数定义所在文件的 MergedIndex

3. MergedIndex.lookup(symbol, RelationKind::Callee, callback)
   → 每个 Relation 的 target_symbol 是被调用者的 SymbolHash
   → Relation 的 range 是调用发生的位置

4. 对每个被调用者 SymbolHash，查 ProjectIndex 获取其名称和定义位置
   → 组装 CallHierarchyOutgoingCall { to: 被调用者, fromRanges: [调用位置] }
```

Outgoing Calls 与 Incoming Calls 的关键区别：Outgoing 只需查询源函数定义所在的文件分片，不需要全局扫描。

#### Type Hierarchy 流程

Type Hierarchy 使用 `Base`/`Derived` 关系。LSP 协议分三步：`prepareTypeHierarchy` → `supertypes` / `subtypes`。

**Prepare**（定位类型符号）：

```
cursor → MergedIndex.lookup(offset) → SymbolHash
→ 确认符号是 Class/Struct/Enum 等类型 (检查 SymbolKind)
→ 返回 TypeHierarchyItem { name, kind, uri, range }
```

**Supertypes**（基类查询）：

```
1. 已知派生类 SymbolHash

2. 找到派生类定义所在文件的 MergedIndex

3. MergedIndex.lookup(symbol, RelationKind::Base, callback)
   → 每个 Relation 的 target_symbol 是基类的 SymbolHash

4. 对每个基类 SymbolHash，查 ProjectIndex 获取名称和定义位置
   → 组装 TypeHierarchyItem
```

Supertypes 查询范围有限——一个类的基类信息只在其定义所在的文件分片中，无需全局搜索。

**Subtypes**（派生类查询）：

```
1. 已知基类 SymbolHash

2. ProjectIndex.symbols[hash].reference_files → 引用文件集合

3. 线程池并行查询:
   MergedIndex.lookup(symbol, RelationKind::Derived, callback)
   → 每个 Relation 的 target_symbol 是派生类的 SymbolHash

4. 对每个派生类 SymbolHash，查 ProjectIndex 获取名称和定义位置
   → 组装 TypeHierarchyItem
```

Subtypes 需要全局搜索（任何文件都可能定义派生类），因此走 `reference_files` bitmap 并行查询路径，与 Find References 相同。

#### PrepareRename 流程

PrepareRename 在主进程处理，不需要转发到 Worker：

```
cursor (文件 F, 字节偏移 offset)
→ MergedIndex(F).lookup(offset) → SymbolHash + range
→ 返回 { range, placeholder: symbol name }
```

仅验证光标位置是否有可重命名的符号并返回其范围和名称，不执行实际重命名。

#### WorkspaceSymbol 流程

WorkspaceSymbol 在主进程处理，基于 ProjectIndex 的全局符号表进行模糊搜索：

```
query string
→ ProjectIndex.symbols 遍历，对每个 Symbol.name 进行模糊匹配
→ 按匹配分数排序，取 top N
→ 对每个匹配的 SymbolHash，查找定义位置:
   ProjectIndex.symbols[hash].reference_files → 找到定义所在文件
   MergedIndex.lookup(symbol, RelationKind::Definition) → 获取定义 range
→ 返回 SymbolInformation[]
```

#### 查询模式总结

| 功能 | RelationKind | 查询范围 | 说明 |
|------|-------------|---------|------|
| Find References | Reference | 全局 (reference_files) | 所有引用位置 |
| Go To Definition | Definition | 全局 (reference_files) | 定义位置 |
| Rename | Reference \| Declaration \| Definition | 全局 (reference_files) | 所有出现位置 → TextEdit |
| Incoming Calls | Caller | 全局 (reference_files) | 谁调用了目标函数 |
| Outgoing Calls | Callee | 局部 (定义所在文件) | 目标函数调用了谁 |
| Supertypes | Base | 局部 (定义所在文件) | 直接基类 |
| Subtypes | Derived | 全局 (reference_files) | 直接派生类 |

所有"全局"查询共享同一模式：`ProjectIndex.symbols[hash].reference_files` → 线程池并行查询各分片。"局部"查询只需查单个分片，开销更小。

#### 查询的线程安全性

- 索引数据存储在主进程内存中
- MergedIndex 在磁盘模式下是只读的（直接读 FlatBuffer），天然线程安全
- 查询在主进程的线程池中并行执行，无需加锁
- 合并新索引数据在事件循环线程中执行，通过 atomic swap 与查询线程隔离（参见 7.2 节）：查询线程持有旧 buffer 的 shared_ptr，merge 完成后 swap 到新 buffer，两者不互斥

#### lookup 实现细节

**Occurrence 查找**（通过字节偏移）：

```cpp
// occurrences 按 range.end 排序
auto it = std::ranges::lower_bound(occurrences, offset, {},
    [](const Occurrence& o) { return o.range.end; });

while (it != occurrences.end()) {
    if (it->range.contains(offset)) {
        if (!callback(*it)) break;  // callback 返回 false 停止
        it++;
    } else {
        break;
    }
}
```

**Relation 查找**（通过符号 + 关系类型）：

```cpp
// 查找指定符号的所有关系，按 RelationKind 过滤
auto it = relations.find(symbol);
if (it != relations.end()) {
    for (auto& [relation, bitmap] : it->second) {
        if (relation.kind & kind) {  // 位运算过滤
            if (!callback(relation)) break;
        }
    }
}
```

两种查找都支持磁盘模式（直接读 FlatBuffer）和内存模式（读 Impl），接口一致。

### 11.8 序列化与持久化

#### FlatBuffers Schema

`src/index/schema.fbs` 定义了所有索引数据的二进制格式。关键定义：

```fbs
// 固定大小结构 (inline)
struct Range          { begin:uint; end:uint; }
struct Occurrence     { range:Range; target:ulong; }
struct Relation       { kind:uint; padding:uint; range:Range; target_symbol:ulong; }
struct IncludeContext { include_id:uint; canonical_id:uint; }
struct PathMapEntry   { source:uint; index:uint; }

// 变长表
table OccurrenceEntry      { occurrence:Occurrence; context:[ubyte]; }  // context = Roaring bitmap
table RelationEntry        { relation:Relation;     context:[ubyte]; }
table SymbolRelationsEntry { symbol:ulong; relations:[RelationEntry]; }
table SymbolEntry          { symbol_id:ulong; symbol:Symbol; }

// 顶层
table ProjectIndex {
    paths:[PathEntry]; indices:[PathMapEntry]; symbols:[SymbolEntry];
}
table MergedIndex {
    max_canonical_id:uint;
    canonical_cache:[CacheEntry];
    header_contexts:[HeaderContextEntry];
    compilation_contexts:[CompilationContextEntry];
    occurrences:[OccurrenceEntry];
    relations:[SymbolRelationsEntry];
}
```

其中 `[ubyte]` 字段存储序列化后的 Roaring Bitmap 二进制数据。

#### Lazy 反序列化策略

MergedIndex 采用 lazy 反序列化，兼顾启动速度和修改能力：

```
加载: 文件 → mmap 或读入 MemoryBuffer → MergedIndex(buffer)
         此时不反序列化，直接从 FlatBuffer 查询（零拷贝）

修改: 收到新的 TUIndex 需要合并
      → load_in_memory() 将 FlatBuffer 反序列化到 Impl
      → 在 Impl 上执行 merge()
      → 后续查询从 Impl 读取

保存: Impl → 序列化为新的 FlatBuffer → 写入磁盘
      → 下次加载时又回到零拷贝模式
```

#### 持久化文件结构

```
<cache_dir>/
├── project.idx       # ProjectIndex (全局符号表 + 路径池)
└── shards/
    ├── <hash1>.idx   # MergedIndex 分片 (每文件一个)
    ├── <hash2>.idx
    └── ...
```

启动时加载 `project.idx` 和所有分片文件，通过 lazy 反序列化快速恢复索引状态，无需重新索引整个项目。

---

## 12. Feature 模块设计

### 12.1 功能分类

| 功能           | Worker 类型    | 需要 AST   | 主要依赖                                        |
| -------------- | -------------- | ---------- | ----------------------------------------------- |
| CodeCompletion | Stateless      | 是(一次性) | `compile/compilation.cpp`                       |
| SignatureHelp  | Stateless      | 是(一次性) | `compile/compilation.cpp`                       |
| Hover          | Stateful       | 是(持续)   | `semantic/ast_utility.h`, `semantic/resolver.h` |
| SemanticTokens | Stateful       | 是(持续)   | `semantic/symbol_kind.h`                        |
| InlayHints     | Stateful       | 是(持续)   | `semantic/ast_utility.h`                        |
| FoldingRange   | Stateful       | 是(持续)   | AST + 预处理指令                                |
| DocumentSymbol | Stateful       | 是(持续)   | AST 遍历                                        |
| DocumentLink   | Stateful       | 是(持续)   | 包含图                                          |
| CodeAction     | Stateful       | 是(持续)   | diagnostics, AST                                |
| Diagnostics    | Stateful       | 编译副产品 | `compile/diagnostic.h`                          |
| Formatting     | 主进程线程池    | 否         | clang-format（避免阻塞事件循环）                 |
| GoToDefinition | 主进程 + Stateful fallback | 否(索引) / 是(fallback) | MergedIndex 优先，找不到时 fallback 到 StatefulWorker AST |
| FindReferences | 主进程 (Server) | 否         | ProjectIndex, MergedIndex（参见 11.7 节）        |
| Rename         | 主进程 (Server) | 否         | ProjectIndex, MergedIndex（参见 11.7 节）        |
| PrepareRename  | 主进程 (Server) | 否         | MergedIndex (lookup by offset)                  |
| WorkspaceSymbol| 主进程 (Server) | 否         | ProjectIndex (模糊搜索 symbols)                  |
| CallHierarchy  | 主进程 (Server) | 否         | ProjectIndex, MergedIndex（参见 11.7 节）        |
| TypeHierarchy  | 主进程 (Server) | 否         | ProjectIndex, MergedIndex（参见 11.7 节）        |
| SelectionRange | Stateful       | 是(持续)   | AST SelectionTree（v2，v1 暂不实现）             |

### 12.2 功能配置选项

```
CodeCompletion:
  - keyword_snippets: bool
  - function_arguments: bool
  - template_arguments: bool
  - insert_parens: bool
  - limit: int

Hover:
  - enable_doxygen_parsing: bool
  - parse_comment_as_markdown: bool
  - show_aka: bool

InlayHints:
  - enable_deduced_types: bool
  - enable_designators: bool
  - type_name_limit: int
```

---

## 13. 配置系统设计

### 13.1 启动参数 (`Options`)

```cpp
struct Options {
    Mode mode;                        // Pipe | Socket | StatelessWorker | StatefulWorker
    std::string host;                 // Socket 模式的地址 (默认 127.0.0.1)
    int port;                         // Socket 模式的端口 (默认 50051)
    std::string self_path;            // 自身可执行文件路径 (用于 spawn worker)

    std::size_t stateless_worker_count;    // 无状态 Worker 数量
    std::size_t stateful_worker_count;     // 有状态 Worker 数量
    std::size_t worker_memory_limit;       // 每个有状态 Worker 内存上限
};
```

**默认值自动计算**：默认值基于系统资源自动计算，用户仍可通过启动参数或 `clice.toml` 手动覆盖：

```
stateful_worker_count  = max(1, min(CPU_cores / 2, available_memory_GB / 4))
stateless_worker_count = max(1, CPU_cores / 4)
  （其中 1 个专用于后台索引）
worker_memory_limit    = available_memory / stateful_worker_count（向下取整到 GB）
```

### 13.2 项目配置 (`clice.toml`)

每个 workspace 根目录一个，使用 eventide 的 serde 框架直接反序列化到 C++ 结构体。

**完整 Schema**：

```toml
# === 编译数据库 ===
# CDB 路径，支持 ${workspace} 变量
compile_database = "${workspace}/build/compile_commands.json"

# === 缓存 ===
# 缓存目录 (默认: ${workspace}/.clice/)
cache_dir = "${workspace}/.clice/"
# PCH/PCM 缓存最大容量 (默认: 10GB)
cache_max_size = "10GB"

# === 编译选项覆盖 ===
[compile_options]
# 从原始编译命令中移除的参数
remove = ["-Werror"]
# 追加到编译命令末尾的参数
append = ["-DCLICE=1"]

# === Worker 配置 ===
[worker]
stateless_count = 0      # 0 = 自动计算
stateful_count = 0       # 0 = 自动计算
memory_limit = "4GB"     # 每个 StatefulWorker 内存上限

# === 功能配置 ===
[feature.completion]
keyword_snippets = false
function_arguments = false
template_arguments = false
insert_parens = false
bundle_overloads = true
limit = 0                # 0 = 无限制

[feature.hover]
doxygen_parsing = true
markdown_comments = true
show_aka = true

[feature.inlay_hints]
enabled = true
parameters = true
deduced_types = true
designators = true
block_end = false
default_arguments = false
type_name_limit = 32
```

### 13.3 缓存目录

缓存目录位置确定规则：

1. `clice.toml` 中的 `cache_dir` 设置（最高优先级）
2. 默认值：`<workspace>/.clice/`

缓存目录结构：

```
<cache_dir>/
├── cache.json        # PCH/PCM 元信息
├── *.pch             # 预编译头文件
├── *.pcm             # 预编译模块文件
├── graph.idx         # FuzzyGraph 模糊包含图缓存
├── project.idx       # ProjectIndex 全局符号表
└── shards/
    ├── <hash1>.idx   # MergedIndex 分片
    └── ...
```

### 13.4 配置热更新

监听 workspace 根目录下的 `clice.toml`、`compile_commands.json`。未来计划通过 watcher 监听整个目录树，统一处理 `.clang-format`、`.clang-tidy` 等配置文件的变更。

**compile_commands.json 变更自动检测**：

```
CDB watcher 检测到变更
  │
  ├─ 重新加载 CDB
  │     └─ 返回 UpdateInfo[] { Inserted, Deleted, Unchanged }
  ├─ 对 Deleted 文件:
  │     ├─ 清除 CompileGraph 中的编译单元
  │     └─ 标记相关索引为过期
  ├─ 对 Inserted 文件:
  │     └─ 加入后台索引队列
  ├─ 对 Unchanged 文件但编译参数变化:
  │     ├─ 失效相关 PCH/PCM 缓存
  │     └─ 标记已打开文件的 build_requested = true
  └─ 触发 FuzzyGraph 增量更新
```

**clice.toml 变更**：重新加载配置，更新功能选项。编译选项变更按 CDB 变更处理。

---

## 14. 错误处理与容错

### 14.1 Worker 崩溃恢复

（参见 4.4 节和 7.4 节已有的崩溃恢复设计）

### 14.2 非崩溃错误处理

除 Worker 崩溃外，系统还需处理以下错误场景：

#### Worker 编译返回 FatalError

```
StatefulWorker 返回 CompileResult { diagnostics: [...], status: FatalError }
  │
  ├─ 主进程发布空 diagnostics (清除旧诊断)
  ├─ 日志记录 FatalError 详情 (spdlog warn)
  ├─ 不重试编译 (避免死循环)
  └─ 用户下次编辑时触发新的 build drain 自动重试
```

#### IPC 请求超时

```
主进程发送请求给 Worker，超过 timeout (默认 30s) 无响应
  │
  ├─ 判定 Worker 可能卡死
  ├─ SIGTERM 旧 Worker 进程
  ├─ spawn 新 Worker 进程
  ├─ 对 StatefulWorker: 清除所有 owner 映射 (同崩溃恢复)
  └─ 重试一次请求
```

#### PCM/PCH 编译失败

```
StatelessWorker 返回 BuildPCHResult/BuildPCMResult { success: false, error: "..." }
  │
  ├─ CompileGraph: 不清除 dirty 标记 (保持 dirty = true)
  │     → 依赖此产物的所有文件编译跳过
  ├─ 通过 LSP window/showMessage 通知用户:
  │     "Failed to build precompiled header/module: <error>"
  └─ 用户修复源文件 → didSave → 自动重试编译
```

#### 错误分类

| 错误类型 | 检测方式 | 处理策略 |
|---------|---------|---------|
| Worker 崩溃 | IPC pipe 断开 / 进程退出 | 重启 Worker + 重试 (参见 4.4 节) |
| Worker 超时 | 请求超时 (30s) | kill + 重启 + 重试 |
| Worker 返回错误 | RPCError / status=FatalError | 日志 + 发布空结果 + 不重试 |
| 编译失败 | success=false | CompileGraph 保持 dirty + 通知用户 |
| 索引失败 | IndexResult.success=false | 日志 + 跳过该文件 + 下次空闲重试 |

---

## 15. 内存监控与后台索引调度

### 15.1 Worker 内存监控

主进程需要监控 StatefulWorker 的内存使用，用于 LRU 驱逐决策（参见 4.3 节）。

**监控方案**：

- **Linux**: 读取 `/proc/<pid>/status` 中的 `VmRSS` 字段
- **Windows**: 调用 `GetProcessMemoryInfo()` 获取 `WorkingSetSize`
- **macOS**: 调用 `proc_pid_rusage()` 获取 `ri_phys_footprint`

**监控频率**：

- 周期性检查: 每 5 秒通过定时器触发
- 事件驱动检查: 在 `assign_worker` 分配新文档时检查
- Worker 自报 (可选): Worker 在编译完成后在响应中附带内存用量

**驱逐触发条件**：

```
if worker_memory(worker_id) > worker_memory_limit:
    evict_oldest_document(worker_id)
if total_worker_memory() > global_memory_limit:
    evict_globally_oldest_document()
```

### 15.2 后台索引空闲检测

索引只在用户空闲时执行，避免干扰交互式操作。

**"用户活动"定义**：收到以下 LSP 消息视为用户活动：
- `textDocument/didChange` (正在编辑)
- `textDocument/completion` (正在补全)
- `textDocument/signatureHelp` (正在查看签名)

以下消息**不算**用户活动：
- `textDocument/didOpen` / `textDocument/didClose` (文件管理操作)
- `textDocument/hover` (被动信息，不阻塞索引)

**空闲检测机制**：

```
用户活动 → 重置 idle_timer (如 3 秒)
  │
  idle_timer 到期 → 进入空闲状态
  │
  ├─ 从索引队列中取出下一个待索引文件
  ├─ 通过 CompileGraph 确保依赖就绪
  ├─ 发送 worker::IndexParams 到低优先级 StatelessWorker
  ├─ 收到 TUIndex → 合并到 ProjectIndex + MergedIndex
  │
  └─ 若用户恢复活动:
        取消正在进行的索引编译 (cancellation token)
        等下次空闲继续 (从队列中取下一个文件)
```

**索引进度报告**：通过 LSP `$/progress` 报告：`"Indexing files... (N/M)"`

---

## 16. LSP Progress 报告

使用 LSP `$/progress` 协议报告长时间操作，让用户了解系统状态。

| 操作 | 进度标题 | 进度详情 | 触发时机 |
|------|---------|---------|---------|
| FuzzyGraph 扫描 | "Scanning includes" | "N/M files" | 首次启动或缓存失效 |
| PCH 构建 | "Building preamble" | 文件名 | didOpen 首次编译 |
| PCM 构建 | "Building module" | 模块名 | 模块依赖编译 |
| 后台索引 | "Indexing" | "N/M files" | 空闲时后台索引 |
| CDB 重载 | "Reloading compile database" | — | compile_commands.json 变更 |

**实现方式**：

```
主进程创建 progress token → $/progress begin
操作执行中 → $/progress report (更新百分比/计数)
操作完成 → $/progress end
```

---

## 17. 日志系统

### 17.1 主进程日志

使用 spdlog 输出到 stderr（Pipe 模式）或日志文件：

- **Error**: 致命错误、Worker 崩溃
- **Warn**: 编译失败、PCM/PCH 构建失败、配置问题
- **Info**: 启动信息、CDB 加载、索引完成统计
- **Debug**: 请求/响应追踪、编译图状态变化
- **Trace**: 详细 IPC 消息内容

### 17.2 Worker 日志

Worker 进程的日志通过以下方式收集：

- Worker 写入 stderr，主进程通过 pipe 读取（eventide 的 process spawn 支持 stderr pipe）
- 主进程将 Worker 日志前缀标记（如 `[SL-0]` / `[SF-1]`）后统一输出
- Worker 崩溃时的最后日志用于诊断

### 17.3 LSP 客户端日志

通过 `window/logMessage` 发送关键日志给 LSP 客户端：

- **Error**: Worker 崩溃、CDB 加载失败
- **Warning**: PCM/PCH 编译失败、头文件找不到
- **Info**: 索引完成、CDB 重载完成

---

## 18. 多编译数据库支持

### 18.1 场景

实际项目可能有多个 `compile_commands.json`：
- `build-debug/compile_commands.json`
- `build-release/compile_commands.json`
- 交叉编译 `build-arm/compile_commands.json`

### 18.2 发现策略

1. `clice.toml` 中显式指定（最高优先级）：
   ```toml
   compile_database = "${workspace}/build-debug/compile_commands.json"
   ```
2. 搜索 workspace 下常见路径：`build/`, `cmake-build-debug/`, `out/`
3. 找到多个时，默认选择最近修改的

### 18.3 用户切换

通过扩展 LSP 命令 `workspace/executeCommand`:
- `clice/switchCompileDatabase`: 列出所有发现的 CDB，用户选择
- 切换后触发完整的 CDB 重载流程（参见 13.4 节）

---

## 19. FuzzyGraph 持久化格式

### 19.1 ContextID 计算

ContextID 通过 hash 影响 include 解析的编译参数确定：

```cpp
ContextID compute_context_id(ArrayRef<const char*> arguments) {
    // 提取 -I, -isystem, -isysroot, --sysroot 等参数
    // 排序后计算 xxh3_64bits hash
    return xxh3_64bits(sorted_include_args);
}
```

典型项目通常只有 2-3 种 ContextID。

### 19.2 FlatBuffers Schema

```fbs
// 添加到 schema.fbs

struct FuzzyEdge {
    target_file_id: uint;
    conditional: bool;
}

table FuzzyFileEntry {
    file_id: uint;
    context_id: ulong;
    includes: [FuzzyEdge];     // 正向边
}

table FuzzyGraph {
    paths: [PathEntry];         // 复用 PathEntry
    contexts: [ulong];          // ContextID 列表
    entries: [FuzzyFileEntry];  // 正向图
    // 反向图在加载时从正向图构建
}
```

### 19.3 增量更新持久化

FuzzyGraph 缓存 (`graph.idx`) 在以下时机写入磁盘：
- 初始扫描完成后
- 定期保存（如每 5 分钟，若有变更）
- 正常关闭时
