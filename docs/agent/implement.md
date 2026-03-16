# clice Server 逐步实现方案

基于 `design.md` 的完整设计，本文档规划一个可落地的分阶段实现路径。每个阶段产出可测试的里程碑，确保增量开发过程中始终有可运行的系统。

**现状评估**：`src/clice.cc` 是一个空壳 `main()`。Compile、Feature、Semantic、Index、Syntax、Support 六个底层模块已实现。缺失的是 Server 层——进程管理、LSP 协议处理、请求路由、Worker 进程通信、CompileGraph 调度。

**依赖关系图**：

```
Phase 0: 基础设施 (CLI, 配置, 日志)
    │
    ▼
Phase 1: 单进程 LSP Server (无 Worker)
    │
    ├── Phase 2a: StatelessWorker 进程
    ├── Phase 2b: StatefulWorker 进程
    │
    ▼
Phase 3: MasterServer + Worker 池 (多进程)
    │
    ▼
Phase 4: CompileGraph 编译调度
    │
    ▼
Phase 5: 后台索引 + 全局查询
    │
    ▼
Phase 6: 高级功能与优化
```

---

## Phase 0: 基础设施

**目标**：可执行文件能解析 CLI 参数、加载配置、初始化日志。

**产出**：`clice --help` 正常工作，`clice --mode=pipe` 启动后能读取 `clice.toml`。

### 0.1 CLI 参数解析

**文件**：`src/server/options.h`, `src/server/options.cpp`

```cpp
struct Options {
    enum class Mode { Pipe, Socket, StatelessWorker, StatefulWorker };

    Mode mode = Mode::Pipe;
    std::string host = "127.0.0.1";
    int port = 50051;
    std::string self_path;              // argv[0] 的绝对路径

    std::size_t stateless_worker_count = 0;  // 0 = 自动
    std::size_t stateful_worker_count = 0;
    std::size_t worker_memory_limit = 0;

    static Options parse(int argc, const char** argv);
    void compute_defaults();            // 基于系统资源计算默认值
};
```

**实现要点**：
- 使用 eventide 的 `deco` (declarative CLI) 模块解析参数
- `compute_defaults()` 读取 CPU 核心数和可用内存，按 design.md 13.1 节公式计算
- `self_path` 通过 `/proc/self/exe` (Linux) / `_NSGetExecutablePath` (macOS) / `GetModuleFileName` (Windows) 获取

### 0.2 配置文件加载

**文件**：`src/server/config.h`, `src/server/config.cpp`

```cpp
struct Config {
    struct Project {
        bool clang_tidy = false;
        std::size_t max_active_file = 8;
        std::string cache_dir = "${workspace}/.clice/cache";
        std::string index_dir = "${workspace}/.clice/index";
        std::string logging_dir = "${workspace}/.clice/logging";
        std::vector<std::string> compile_commands_paths = {"${workspace}/build"};
    } project;

    struct Rule {
        std::vector<std::string> patterns;
        std::vector<std::string> append;
        std::vector<std::string> remove;
    };
    std::vector<Rule> rules;

    static Config load(llvm::StringRef workspace_root);
};
```

**实现要点**：
- 使用 eventide 的 `serde` 框架将 TOML 直接反序列化到 `Config`
- `${workspace}`, `${version}`, `${llvm_version}` 变量替换在 `load()` 中处理
- 找不到 `clice.toml` 时使用全默认值
- `[[rules]]` 是有序数组，按声明顺序匹配（参见 `docs/clice.toml`）

### 0.3 更新 main()

**文件**：`src/clice.cc`

```cpp
int main(int argc, const char** argv) {
    auto options = Options::parse(argc, argv);
    options.compute_defaults();

    switch (options.mode) {
        case Options::Mode::Pipe:
            return run_pipe_mode(options);
        case Options::Mode::Socket:
            return run_socket_mode(options);
        case Options::Mode::StatelessWorker:
            return run_stateless_worker_mode(options);
        case Options::Mode::StatefulWorker:
            return run_stateful_worker_mode(options);
    }
}
```

### 0.4 测试

- 单元测试：`Options::parse` 各种参数组合
- 单元测试：`Config::load` 正常/缺失/畸形 TOML
- 集成测试：`clice --help` 退出码 0

---

## Phase 1: 单进程 LSP Server

**目标**：clice 作为单进程 LSP Server 运行，不 spawn Worker 进程。在主进程内直接编译文件、响应 LSP 请求。这验证了 LSP 协议处理、文档状态管理、Peer 集成的正确性。

**产出**：用 VS Code 连接 clice，能打开文件看到 diagnostics、hover、补全。

### 1.1 Pipe 模式启动

**文件**：`src/server/pipe_mode.cpp`

```cpp
int run_pipe_mode(const Options& options) {
    et::event_loop loop;
    auto transport = et::StreamTransport::open_stdio(loop).value();
    auto peer = et::JsonPeer(loop, std::move(transport));

    MasterServer server(loop, peer, options);
    server.register_callbacks();

    loop.schedule(peer.run());
    loop.run();
    return 0;
}
```

**实现要点**：
- `StreamTransport::open_stdio()` 打开 stdin/stdout
- `JsonPeer` 处理 JSON-RPC 2.0 协议
- 暂时不 spawn Worker，所有编译在主进程内完成

### 1.2 MasterServer 骨架

**文件**：`src/server/master_server.h`, `src/server/master_server.cpp`

```cpp
class MasterServer {
    et::event_loop& loop;
    et::JsonPeer& peer;
    Options options;

    enum class State { Uninitialized, Initialized, Ready, ShuttingDown };
    State state = State::Uninitialized;

    std::string workspace_root;
    Config config;
    CompilationDatabase cdb;

    ServerPathPool path_pool;  // 全局路径池
    llvm::DenseMap<std::uint32_t, DocumentState> documents;  // path_id → DocumentState

public:
    MasterServer(et::event_loop& loop, et::JsonPeer& peer, const Options& options);

    void register_callbacks();

private:
    // LSP lifecycle
    et::RequestResult<protocol::InitializeParams>
        on_initialize(auto& ctx, const protocol::InitializeParams& params);
    void on_initialized(const protocol::InitializedParams& params);
    et::RequestResult<protocol::ShutdownParams> on_shutdown(auto& ctx);
    void on_exit();

    // Document sync
    void on_did_open(const protocol::DidOpenTextDocumentParams& params);
    void on_did_change(const protocol::DidChangeTextDocumentParams& params);
    void on_did_save(const protocol::DidSaveTextDocumentParams& params);
    void on_did_close(const protocol::DidCloseTextDocumentParams& params);
};
```

**分步实现**：

1. **ServerPathPool**（`src/server/path_pool.h`）：实现全局路径池（`llvm::BumpPtrAllocator` + `llvm::StringMap` + `llvm::SmallVector`），所有路径通过 `intern()` 获取 `uint32_t` ID，后续各模块统一使用 path_id
2. **initialize / shutdown / exit**：实现生命周期状态机，返回 server capabilities
3. **didOpen / didChange / didClose**：实现 `DocumentState` 管理（version, text, generation），使用 `path_pool.intern()` 作为 key
4. **incremental text sync**：实现 `apply_content_changes()` 将 LSP diff 应用到全量 text

### 1.3 单进程编译路径

Phase 1 中暂不使用 Worker 进程，直接在主进程中编译：

```cpp
et::task<> MasterServer::run_build_drain_inline(std::uint32_t path_id) {
    auto it = documents.find(path_id);
    if (it == documents.end()) co_return;
    it->second.build_running = true;

    while (it->second.build_requested) {
        it->second.build_requested = false;
        auto gen = it->second.generation;
        auto path = path_pool.resolve(path_id);

        auto result = co_await et::queue([&]() {
            CompilationParams params;
            params.kind = CompilationKind::Parse;
            params.add_remapped_file(path, it->second.text);
            auto unit = compile(params);
            return feature::diagnostics(unit);
        }, loop);

        it = documents.find(path_id);
        if (it == documents.end()) co_return;
        if (it->second.generation != gen) continue;
        publish_diagnostics(path_id, result);
    }

    it->second.build_running = false;
}
```

### 1.4 Feature 请求 (Hover, Completion)

在单进程模式下，直接调用现有的 feature 函数：

```cpp
et::RequestResult<protocol::HoverParams>
MasterServer::on_hover(auto& ctx, const protocol::HoverParams& params) {
    // 1. 确保文件已编译
    // 2. 在线程池中执行 hover
    auto result = co_await et::queue([&]() {
        return feature::hover(compilation_unit, offset);
    }, loop);
    co_return result;
}
```

### 1.5 测试

- Python 集成测试：复用现有 `tests/integration/` 框架
- 测试 initialize → didOpen → hover → shutdown → exit 完整流程
- 测试 incremental didChange 文本同步

---

## Phase 2a: StatelessWorker 进程

**目标**：实现 StatelessWorker 作为独立进程运行，能接收 bincode JSON-RPC 请求并执行一次性编译任务。

**产出**：可以单独启动 `clice --mode=stateless-worker` 并通过 stdin/stdout 与之通信。

### 2a.1 Worker 进程入口

**文件**：`src/server/stateless_worker.h`, `src/server/stateless_worker.cpp`

```cpp
class StatelessWorker {
    et::BincodePeer& peer;

public:
    StatelessWorker(et::BincodePeer& peer);
    void register_callbacks();

private:
    // 编译请求处理
    et::RequestResult<worker::BuildPCHParams> on_build_pch(auto& ctx, auto& params);
    et::RequestResult<worker::BuildPCMParams> on_build_pcm(auto& ctx, auto& params);
    et::RequestResult<worker::CompletionParams> on_completion(auto& ctx, auto& params);
    et::RequestResult<worker::SignatureHelpParams> on_signature_help(auto& ctx, auto& params);
    et::RequestResult<worker::IndexParams> on_index(auto& ctx, auto& params);
};

int run_stateless_worker_mode(const Options& options) {
    et::event_loop loop;
    auto transport = et::StreamTransport::open_stdio(loop).value();
    auto peer = et::BincodePeer(loop, std::move(transport));

    StatelessWorker worker(peer);
    worker.register_callbacks();

    loop.schedule(peer.run());
    loop.run();
    return 0;
}
```

### 2a.2 Peer 取消到 atomic_bool 桥接

每个请求处理回调中实现取消桥接：

```cpp
et::RequestResult<worker::BuildPCHParams>
StatelessWorker::on_build_pch(auto& ctx, const worker::BuildPCHParams& params) {
    auto stop = std::make_shared<std::atomic_bool>(false);

    // 桥接 Peer 取消 → clang stop 标志
    loop.schedule([](et::cancellation_token token,
                     std::shared_ptr<std::atomic_bool> s) -> et::task<> {
        co_await token.wait();
        s->store(true);
    }(ctx.cancellation, stop));

    auto result = co_await et::queue([&]() {
        CompilationParams compile_params;
        compile_params.stop = stop;
        // ... 构造参数 ...
        auto unit = compile(compile_params);
        return worker::BuildPCHResult { .success = true };
    }, loop);

    co_return result;
}
```

### 2a.3 IPC 消息类型定义

**文件**：`src/server/protocol.h`

定义所有 `worker::*` 消息结构体（参见 design.md 6.1 节），并为每个添加 eventide 的 `RequestTraits` 特化：

```cpp
namespace worker {

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

} // namespace worker

// eventide RequestTraits 特化
template<> struct et::protocol::RequestTraits<worker::BuildPCHParams> {
    using Result = worker::BuildPCHResult;
    static constexpr auto method = "clice/worker/buildPCH";
};
```

### 2a.4 测试

- 单元测试：启动 StatelessWorker 进程，通过 pipe 发送 BuildPCH 请求，验证 PCH 文件生成
- 测试取消：发送请求后立即取消，验证 Worker 正确停止
- 测试崩溃恢复：向 Worker 发送导致 crash 的输入，验证进程可重启

---

## Phase 2b: StatefulWorker 进程

**目标**：实现 StatefulWorker 作为独立进程运行，能持有 AST 并处理连续的 feature 请求。

**产出**：可以单独启动 `clice --mode=stateful-worker` 并处理 compile → hover → semanticTokens 序列。

### 2b.1 Worker 进程与 DocumentEntry

**文件**：`src/server/stateful_worker.h`, `src/server/stateful_worker.cpp`

```cpp
class StatefulWorker {
    et::BincodePeer& peer;
    std::size_t memory_limit;

    struct DocumentEntry {
        int version;
        std::string text;
        CompilationUnit unit;
        std::atomic<bool> dirty{false};

        std::string directory;
        std::vector<std::string> arguments;
        std::pair<std::string, uint32_t> pch;
        llvm::StringMap<std::string> pcms;

        et::mutex strand_mutex;  // per-document 串行化
    };

    llvm::StringMap<DocumentEntry> documents;

    // LRU 管理
    std::list<llvm::StringRef> lru;
    llvm::StringMap<std::list<llvm::StringRef>::iterator> lru_index;

public:
    StatefulWorker(et::BincodePeer& peer, std::size_t memory_limit);
    void register_callbacks();

private:
    et::RequestResult<worker::CompileParams> on_compile(auto& ctx, auto& params);
    et::RequestResult<worker::HoverParams> on_hover(auto& ctx, auto& params);
    et::RequestResult<worker::SemanticTokensParams> on_semantic_tokens(auto& ctx, auto& params);
    // ... 其他 feature handlers ...

    void on_document_update(const worker::DocumentUpdateParams& params);
    void on_evict(const worker::EvictParams& params);

    void touch_lru(const std::string& uri);
    void shrink_if_over_limit();
};
```

### 2b.2 Compile 请求处理

```cpp
et::RequestResult<worker::CompileParams>
StatefulWorker::on_compile(auto& ctx, const worker::CompileParams& params) {
    auto& doc = documents[params.uri];
    doc.version = params.version;
    doc.text = params.text;
    doc.directory = params.directory;
    doc.arguments = params.arguments;
    doc.pch = params.pch;
    doc.pcms = params.pcms;

    touch_lru(params.uri);

    // per-document 串行化
    co_await doc.strand_mutex.lock();

    auto result = co_await et::queue([&]() {
        CompilationParams cp;
        cp.kind = CompilationKind::Parse;
        // ... 从 doc 构造编译参数 ...
        cp.add_remapped_file(path, doc.text, doc.pch.second);
        doc.unit = compile(cp);
        doc.dirty = false;
        return worker::CompileResult{
            .uri = params.uri,
            .version = params.version,
            .diagnostics = feature::diagnostics(doc.unit),
            .memory_usage = current_memory_usage()
        };
    }, loop);

    doc.strand_mutex.unlock();
    co_return result;
}
```

### 2b.3 Feature 请求处理（以 Hover 为例）

```cpp
et::RequestResult<worker::HoverParams>
StatefulWorker::on_hover(auto& ctx, const worker::HoverParams& params) {
    auto it = documents.find(params.uri);
    if (it == documents.end()) {
        co_return et::outcome_error(et::error{-1, "document not found"});
    }
    auto& doc = it->second;
    touch_lru(params.uri);

    co_await doc.strand_mutex.lock();

    // 在线程池中执行 AST 遍历
    auto result = co_await et::queue([&]() -> std::string {
        if (doc.dirty.load()) return "null";

        auto hover = feature::hover(doc.unit, offset);
        // 透传模式：序列化为 JSON 字符串
        return serde::json::to_string(hover);
    }, loop);

    doc.strand_mutex.unlock();
    co_return result;
}
```

### 2b.4 LRU 驱逐与 Worker → Master 通知

```cpp
void StatefulWorker::shrink_if_over_limit() {
    while (current_memory_usage() > memory_limit && !lru.empty()) {
        auto& oldest_uri = lru.back();

        auto it = documents.find(oldest_uri);
        if (it != documents.end()) {
            it->second.unit = {};  // 释放 AST
            documents.erase(it);
        }

        // 通知主进程
        peer.send_notification(worker::EvictedParams{ .uri = oldest_uri });

        lru_index.erase(oldest_uri);
        lru.pop_back();
    }
}
```

### 2b.5 测试

- 端到端测试：compile → hover → semanticTokens 序列
- 测试 documentUpdate 后 dirty 标记
- 测试 LRU 驱逐在内存限制下的行为
- 测试 strand 串行化：并发发送 compile + hover

---

## Phase 3: MasterServer + Worker 池

**目标**：主进程 spawn Worker 子进程，通过 bincode JSON-RPC 通信。实现统一的 WorkerPool（内部管理 stateless + stateful workers）。

**产出**：完整的多进程 LSP Server，用户体验与 Phase 1 相同但更稳定（崩溃隔离）。

### 3.1 进程 Spawn

**文件**：`src/server/worker_pool.h`, `src/server/worker_pool.cpp`

```cpp
struct WorkerProcess {
    et::process proc;
    et::BincodePeer peer;
    std::unique_ptr<et::StreamTransport> transport;
};

et::task<WorkerProcess> spawn_worker(const Options& options,
                                      Options::Mode mode,
                                      et::event_loop& loop) {
    et::process::options opts;
    opts.file = options.self_path;
    opts.args = {opts.file};

    if (mode == Options::Mode::StatelessWorker) {
        opts.args.push_back("--mode=stateless-worker");
    } else {
        opts.args.push_back("--mode=stateful-worker");
        opts.args.push_back("--worker-memory-limit=" +
                            std::to_string(options.worker_memory_limit));
    }

    opts.streams = {
        et::process::stdio::pipe(true, false),   // stdin: parent writes
        et::process::stdio::pipe(false, true),    // stdout: parent reads
        et::process::stdio::pipe(false, true),    // stderr: parent reads (日志)
    };

    auto spawn = et::process::spawn(opts, loop).value();

    auto transport = std::make_unique<et::StreamTransport>(
        std::move(spawn.stdout_pipe),
        std::move(spawn.stdin_pipe)
    );
    auto peer = et::BincodePeer(loop, std::move(transport));

    // 异步读取 stderr 用于日志收集
    loop.schedule(collect_worker_log(std::move(spawn.stderr_pipe), worker_id));

    return WorkerProcess{
        .proc = std::move(spawn.proc),
        .peer = std::move(peer),
        .transport = std::move(transport)
    };
}
```

### 3.2 统一 WorkerPool

对外暴露统一接口，内部管理 stateless 和 stateful 两组进程：

```cpp
class WorkerPool {
    struct StatelessState {
        std::vector<WorkerProcess> workers;
        std::size_t next = 0;  // round-robin
    } stateless;

    struct StatefulState {
        std::vector<WorkerProcess> workers;
        llvm::DenseMap<std::uint32_t, std::size_t> owner;  // path_id → worker index
        std::list<std::uint32_t> owner_lru;
        llvm::DenseMap<std::uint32_t,
            std::list<std::uint32_t>::iterator> owner_lru_index;
    } stateful;

    const Options& options;
    et::event_loop& loop;

public:
    WorkerPool(const Options& opts, et::event_loop& lp);
    et::task<> start();
    et::task<> stop();

    // 无状态请求：round-robin 分发
    template<typename Params>
    auto send_stateless(const Params& params, et::request_options opts = {}) {
        auto& w = stateless.workers[stateless.next++ % stateless.workers.size()];
        return w.peer.send_request(params, opts);
    }

    // 有状态请求：按 path_id 路由（自动分配 Worker）
    template<typename Params>
    auto send_stateful(std::uint32_t path_id, const Params& params,
                       et::request_options opts = {}) {
        auto idx = assign_worker(path_id);
        return stateful.workers[idx].peer.send_request(params, opts);
    }

private:
    std::size_t assign_worker(std::uint32_t path_id);
    et::task<> restart_worker(bool is_stateful, std::size_t index);
    void clear_owner(std::size_t worker_index);
    void remove_owner(std::uint32_t path_id);
};
```

### 3.3 MasterServer 改造

将 Phase 1 的单进程路径替换为统一 WorkerPool 调用：

```cpp
// Phase 1: 直接编译
auto result = co_await et::queue([&](){ return compile(params); }, loop);

// Phase 3: 发送到 Worker（调用方无需关心是哪种 Worker）
auto result = co_await pool.send_stateful(path_id,
    worker::CompileParams{...});
auto pch_result = co_await pool.send_stateless(
    worker::BuildPCHParams{...});
```

### 3.4 崩溃恢复

```cpp
template<typename Params>
auto WorkerPool::send_with_retry(std::uint32_t path_id, const Params& params)
    -> et::task<typename et::protocol::RequestTraits<Params>::Result> {
    try {
        co_return co_await send_stateful(path_id, params);
    } catch (...) {
        auto idx = stateful.owner[path_id];
        co_await restart_worker(true, idx);
        clear_owner(idx);
        co_return co_await send_stateful(path_id, params);
    }
}
```

### 3.6 Worker 日志收集

```cpp
et::task<> collect_worker_log(et::pipe stderr_pipe, std::string prefix) {
    while (true) {
        auto chunk = co_await stderr_pipe.read_chunk();
        if (!chunk.has_value()) break;
        // 前缀标记后统一输出
        spdlog::debug("[{}] {}", prefix, chunk.data());
        stderr_pipe.consume(chunk.size());
    }
}
```

### 3.7 测试

- 集成测试：spawn Worker → 发送请求 → 收到响应
- 崩溃恢复测试：kill Worker 进程 → 验证自动重启和重试
- LRU 驱逐测试：打开超过容量的文件 → 验证旧文档被驱逐
- 内存监控测试：验证 Worker 内存超限时触发驱逐

---

## Phase 4: CompileGraph 编译调度

**目标**：实现 CompileGraph，管理 PCH/PCM 依赖编译。替换 Phase 3 中的手动依赖管理。

**产出**：打开使用 C++20 Modules 的项目时，依赖自动编译，用户无感。

### 4.1 CompileGraph 核心

**文件**：`src/server/compile_graph.h`, `src/server/compile_graph.cpp`

实现 design.md 8.4 节的完整 `CompileGraph`，包含：

- `register_unit(path_id, deps)` — 注册编译单元和依赖（全部使用 `uint32_t` path_id）
- `compile(path_id)` — 编译入口，确保依赖就绪
- `update(path_id)` — 级联失效
- `compile_impl(path_id)` — 编译单个 PCM/PCH（含循环检测）

关键注意事项：
- 使用 `llvm::DenseMap<uint32_t, CompileUnit>` 存储，`ServerPathPool` 管理路径映射
- `compile_impl` 的 `dispatch_compile()` 调用 `pool.send_stateless()` 发送 BuildPCH/BuildPCM 任务
- 使用 `et::with_token` 和 `catch_cancel()` 处理取消
- 用 `units.find()` 替代 `units[]` 避免默认插入

### 4.2 Build Drain 集成

更新 `run_build_drain` 以使用 CompileGraph：

```cpp
et::task<> MasterServer::run_build_drain(std::uint32_t path_id) {
    auto it = documents.find(path_id);
    if (it == documents.end()) co_return;
    it->second.build_running = true;

    while (true) {
        it = documents.find(path_id);
        if (it == documents.end()) break;
        auto& doc = it->second;
        if (!doc.build_requested) { doc.build_running = false; break; }
        doc.build_requested = false;
        auto gen = doc.generation;

        auto deps_ok = co_await compile_graph.compile(path_id, loop)
                            .catch_cancel();
        if (deps_ok.is_cancelled() || !deps_ok.value()) continue;

        auto compile_result = co_await pool.send_stateful(path_id,
            worker::CompileParams{
                .uri = path_pool.resolve(path_id).str(),
                .version = doc.version,
                .text = doc.text,
                .directory = /* from CDB */,
                .arguments = /* from CDB */,
                .pch = compile_graph.get_pch(path_id),
                .pcms = compile_graph.get_pcms(path_id),
            });

        it = documents.find(path_id);
        if (it == documents.end()) break;
        if (it->second.generation != gen) continue;

        publish_diagnostics(path_id, compile_result.diagnostics);
    }
}
```

### 4.3 Debounce 定时器

```cpp
void MasterServer::schedule_build(std::uint32_t path_id) {
    auto it = documents.find(path_id);
    if (it == documents.end()) return;
    auto& doc = it->second;
    doc.build_requested = true;

    if (doc.build_running) return;

    if (!doc.debounce_timer) {
        doc.debounce_timer = std::make_unique<et::timer>(loop);
    }
    doc.debounce_timer->start(std::chrono::milliseconds(200),
        [this, path_id]() {
            loop.schedule(run_build_drain(path_id));
        });
}
```

### 4.4 依赖扫描集成

```cpp
void MasterServer::on_did_open(const protocol::DidOpenTextDocumentParams& params) {
    auto path_id = path_pool.intern(params.textDocument.uri);
    // ... 创建 DocumentState ...

    auto scan_result = scan(doc.text);
    auto dep_ids = resolve_dependencies(path_id, scan_result, cdb, path_pool);

    compile_graph.register_unit(path_id, dep_ids);
    schedule_build(path_id);
}
```

### 4.5 缓存管理

**文件**：`src/server/cache_manager.h`, `src/server/cache_manager.cpp`

```cpp
class CacheManager {
    std::string cache_dir;
    std::size_t max_size;

public:
    void initialize(const Config& config, llvm::StringRef workspace);

    std::string pch_path(llvm::StringRef source_file);
    std::string pcm_path(llvm::StringRef module_name);

    bool is_fresh(llvm::StringRef path);  // mtime + SHA256 检查
    void evict_if_over_limit();           // LRU by access time

    void save_metadata();   // cache.json
    void load_metadata();
};
```

### 4.6 测试

- 单元测试：CompileGraph 依赖解析、编译去重、取消传播
- 单元测试：循环依赖检测
- 集成测试：打开使用 module 的项目，验证 PCM 自动编译
- 集成测试：编辑模块文件 → 验证依赖链重建

---

## Phase 5: 后台索引 + 全局查询

**目标**：实现后台索引系统，支持 FindReferences、GoToDefinition、Rename 等全局查询。

**产出**：在空闲时自动索引项目，支持跨文件的符号查询。

### 5.1 FuzzyGraph 构建

**文件**：`src/server/fuzzy_graph_builder.h`

```cpp
class FuzzyGraphBuilder {
    ServerPathPool& path_pool;
    FuzzyGraph graph;  // llvm::DenseMap<uint32_t, ...> 存储

public:
    explicit FuzzyGraphBuilder(ServerPathPool& pool) : path_pool(pool), graph{pool} {}

    et::task<> scan_all(CompilationDatabase& cdb, et::event_loop& loop);

    void update_file(std::uint32_t path_id, const ScanResult& result);

    llvm::SmallVector<std::uint32_t> get_reverse_includes(std::uint32_t header_id);

    void save(llvm::StringRef path);
    void load(llvm::StringRef path);
};
```

### 5.2 索引调度器

**文件**：`src/server/index_scheduler.h`

```cpp
class IndexScheduler {
    llvm::SmallVector<std::uint32_t> queue;  // 待索引 path_ids
    et::timer idle_timer;
    bool indexing = false;

    WorkerPool& pool;
    CompileGraph& compile_graph;
    ProjectIndex& project_index;
    ServerPathPool& path_pool;

public:
    void enqueue(std::uint32_t path_id);
    void on_user_activity();

    et::task<> run_idle_indexing(et::event_loop& loop);

private:
    et::task<> index_one_file(std::uint32_t path_id);
};
```

### 5.3 索引优先级

```cpp
void IndexScheduler::build_initial_queue(CompilationDatabase& cdb,
                                          FuzzyGraph& graph) {
    auto files = cdb.files();

    // 按优先级排序
    std::stable_sort(files.begin(), files.end(), [&](auto& a, auto& b) {
        // 1. 用户最近打开过的文件优先
        // 2. 被频繁 include 的头文件优先（入度高）
        // 3. 最近修改的文件优先
        auto a_score = compute_priority(a, graph);
        auto b_score = compute_priority(b, graph);
        return a_score > b_score;
    });

    for (auto& f : files) queue.push_back(f);
}
```

### 5.4 全局查询实现

以 FindReferences 为例（在 libuv 线程池中执行，避免阻塞事件循环）：

```cpp
et::RequestResult<protocol::ReferenceParams>
MasterServer::on_find_references(auto& ctx, const protocol::ReferenceParams& params) {
    auto path_id = path_pool.intern(uri_to_path(params.textDocument.uri));
    auto offset = position_to_offset(path_id, params.position);

    // 所有索引查询都在线程池中执行（不仅限于 Formatting）
    auto result = co_await et::queue([&]() {
        std::vector<protocol::Location> locations;

        auto& shard = get_merged_index(path_id);
        SymbolHash hash;
        shard.lookup(offset, [&](const Occurrence& occ) {
            hash = occ.target;
            return false;
        });

        auto& sym = project_index.symbols[hash];

        sym.reference_files.iterate([&](uint32_t file_id) {
            auto file_path = path_pool.resolve(file_id);
            auto& file_shard = get_merged_index(file_id);
            file_shard.lookup(hash, RelationKind::Reference, [&](const Relation& rel) {
                locations.push_back(to_lsp_location(file_path, rel.range));
                return true;
            });
        });

        return locations;
    }, loop);

    co_return result;
}
```

> **注意**：所有不需要 AST 的全局查询（FindReferences, GoToDefinition (索引路径), Rename, WorkspaceSymbol, CallHierarchy, TypeHierarchy）以及 Formatting 都在 libuv 线程池 (`et::queue`) 中执行，避免阻塞主事件循环。

### 5.5 Atomic Swap 并发隔离

更新 MergedIndex 以支持 atomic swap（参见 design.md 7.2 节）：

```cpp
class ConcurrentMergedIndex {
    std::shared_ptr<llvm::MemoryBuffer> buffer;

public:
    // 查询线程调用（线程安全）
    auto snapshot() {
        return std::atomic_load(&buffer);
    }

    // 事件循环线程调用
    void swap_buffer(std::shared_ptr<llvm::MemoryBuffer> new_buffer) {
        std::atomic_store(&buffer, std::move(new_buffer));
    }
};
```

### 5.6 测试

- 索引构建测试：索引一个 TU → 验证 ProjectIndex 和 MergedIndex 更新
- FindReferences 测试：定义符号 → 多处引用 → 验证全部找到
- Rename 测试：重命名符号 → 验证所有引用位置正确
- 并发测试：索引合并与查询并行 → 验证无数据竞争

---

## Phase 6: 高级功能与优化

**目标**：完善所有 LSP 功能，优化性能和用户体验。

### 6.1 Header Context

实现 design.md 第 10 节的非自包含头文件支持：

- `IncludeQueryService`：统一查询接口
- Preamble + Suffix 生成
- Host 源文件选择策略

### 6.2 配置热更新

```cpp
et::task<> MasterServer::watch_config_files() {
    auto cdb_watcher = et::fs_event::start(cdb_path, loop);
    auto toml_watcher = et::fs_event::start(toml_path, loop);

    while (true) {
        auto [event] = co_await et::when_any(
            cdb_watcher.wait(),
            toml_watcher.wait()
        );

        if (cdb_changed) {
            co_await reload_compilation_database();
        } else {
            reload_config();
        }
    }
}
```

### 6.3 多 CDB 支持

- CDB 发现策略
- `clice/switchCompileDatabase` 命令

### 6.4 内存监控

**文件**：`src/server/memory_monitor.h`

```cpp
class MemoryMonitor {
public:
    // 平台相关的内存查询
    static std::size_t get_process_memory(int pid);

    // 周期性检查
    et::task<> run(WorkerPool& pool, et::event_loop& loop,
                   std::chrono::seconds interval = std::chrono::seconds(5));
};
```

### 6.5 Socket 模式

```cpp
int run_socket_mode(const Options& options) {
    et::event_loop loop;
    auto acceptor = et::tcp_socket::listen(options.host, options.port, {}, loop);

    while (true) {
        auto conn = co_await acceptor.accept();
        auto transport = std::make_unique<et::StreamTransport>(std::move(conn));
        auto peer = et::JsonPeer(loop, std::move(transport));

        // 每个连接一个 MasterServer 实例
        loop.schedule(run_master_session(loop, std::move(peer), options));
    }
}
```

### 6.6 LSP Progress 报告

```cpp
class ProgressReporter {
    et::JsonPeer& peer;
    std::string token;

public:
    et::task<> begin(const std::string& title);
    void report(const std::string& message, std::optional<int> percentage = {});
    void end(const std::string& message = "");
};
```

### 6.7 性能优化

- **Completion/SignatureHelp Preamble 变更检测**：避免不必要的 PCH 重建
- **索引 lazy 反序列化**：启动时不反序列化 MergedIndex，查询时按需
- **透传优化**：Feature 请求结果作为 opaque JSON 透传，跳过主进程反序列化
- **并行扫描**：FuzzyGraph 初始扫描使用多个 StatelessWorker 并行处理

---

## 附录 A: 文件清单

```
src/server/
├── options.h / .cpp           # Phase 0: CLI 参数
├── config.h / .cpp            # Phase 0: clice.toml 配置
├── path_pool.h                # Phase 1: ServerPathPool (全局路径池)
├── master_server.h / .cpp     # Phase 1→3: 主服务器
├── pipe_mode.cpp              # Phase 1: Pipe 模式入口
├── socket_mode.cpp            # Phase 6: Socket 模式入口
├── protocol.h                 # Phase 2: IPC 消息定义
├── stateless_worker.h / .cpp  # Phase 2a: 无状态 Worker
├── stateful_worker.h / .cpp   # Phase 2b: 有状态 Worker
├── worker_pool.h / .cpp       # Phase 3: 统一 WorkerPool (内部管理 stateless + stateful)
├── compile_graph.h / .cpp     # Phase 4: 编译依赖图 (DenseMap<uint32_t, CompileUnit>)
├── cache_manager.h / .cpp     # Phase 4: PCH/PCM 缓存管理
├── fuzzy_graph_builder.h/.cpp # Phase 5: 模糊包含图 (DenseMap<uint32_t, ...>)
├── index_scheduler.h / .cpp   # Phase 5: 后台索引调度
├── memory_monitor.h / .cpp    # Phase 6: 内存监控
└── progress.h / .cpp          # Phase 6: LSP Progress
```

## 附录 B: 里程碑与预估工期

| Phase | 里程碑 | 核心交付 | 预估 |
|-------|--------|---------|------|
| 0 | 可启动 | CLI + 配置 + 日志 | 1 周 |
| 1 | 单进程 LSP | 打开文件有 diagnostics + hover + 补全 | 2-3 周 |
| 2a | StatelessWorker | PCH/PCM 编译 + Completion 转发 | 1-2 周 |
| 2b | StatefulWorker | AST 持有 + Feature 请求处理 | 1-2 周 |
| 3 | 多进程 | Worker 池 + 崩溃恢复 + LRU | 2 周 |
| 4 | 编译调度 | CompileGraph + 依赖自动编译 | 2-3 周 |
| 5 | 后台索引 | FindReferences + GoToDef + Rename | 2-3 周 |
| 6 | 完善 | Header Context + 热更新 + 优化 | 3-4 周 |

**总计**：约 14-20 周（一人全职）

## 附录 C: 每阶段可验证标准

| Phase | 验证方式 |
|-------|---------|
| 0 | `clice --help` 输出帮助；`clice --mode=pipe` 启动不崩溃 |
| 1 | VS Code 连接后：打开 .cpp 文件 3s 内显示 diagnostics；hover 显示类型信息；补全弹出建议 |
| 2a | 手动发送 bincode BuildPCH 请求 → .pch 文件生成；取消请求 → 编译停止 |
| 2b | 手动发送 compile → hover 序列 → 返回正确 hover 信息 |
| 3 | Worker 进程崩溃 → 自动重启 → 下次请求正常；LRU 驱逐后 → Worker 释放内存 |
| 4 | 打开使用 module 的项目 → PCM 自动编译 → diagnostics 正常；编辑模块文件 → 依赖链重建 |
| 5 | 项目空闲 3s 后开始索引（Progress 报告）；FindReferences 跨文件查找正确 |
| 6 | 修改 .clang-format → 下次格式化使用新配置；修改 CDB → 自动重载 |
