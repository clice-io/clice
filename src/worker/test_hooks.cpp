module;

#ifndef _WIN32
#include <signal.h>
#endif

module clice;

import :support.process;
import :vfs.file_system;
import :worker.protocol;
import :worker.test_hooks;

namespace clice {

namespace {

/// Calls itself until the thread's stack runs out: the frame's array and
/// the work after the call keep it from becoming a loop, and the test of
/// `depth`, never false, from a recursion clang sees as infinite.
[[gnu::noinline]] int exhaust_stack(volatile int& depth) {
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    depth += 1;
    if(depth > 0) {
        return exhaust_stack(depth) + frame[0];
    }
    return 0;
}

/// Crash the way `kind` names, as real crashes do: a trap, a write to a
/// null pointer, abort(), LLVM's fatal error, a stack overflow, or a trap
/// on a thread the request started.
[[noreturn]] void crash(llvm::StringRef kind) {
    if(kind == "segv") {
        *static_cast<volatile int*>(nullptr) = 1;
    } else if(kind == "abort") {
        std::abort();
    } else if(kind == "fatal") {
        llvm::report_fatal_error("a test hook's fatal error");
    } else if(kind == "overflow") {
        volatile int depth = 0;
        exhaust_stack(depth);
    } else if(kind == "thread") {
        auto thread = sys::Thread::start(1u << 20, [] { LLVM_BUILTIN_TRAP; });
        thread->join();
    }
    LLVM_BUILTIN_TRAP;
}

/// The enumerator `names` spells `name` for.
template <typename Enum, std::size_t N>
std::optional<Enum> parse_name(const std::array<llvm::StringLiteral, N>& names,
                               llvm::StringRef name) {
    auto it = ranges::find(names, name);
    if(it == names.end()) {
        return std::nullopt;
    }
    return static_cast<Enum>(it - names.begin());
}

/// Tell the worker a gate went to that it is released.
void release_gate(const TestHooks::Gate& gate) {
    if(auto worker = gate.worker.lock()) {
        worker->send_notification(worker::GateReleaseParams{gate.id});
    }
}

struct WorkerGate {
    std::uint64_t id;
    std::string tag;
    bool parked = false;
    bool released = false;
    /// Reports the park from the parked thread to the loop; destroyed with
    /// the gate's release, since it keeps the loop alive.
    async::relay relay;
};

/// The gates the master placed in this worker. The mutex also orders a
/// relay's send before its destruction.
struct WorkerGates {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<WorkerGate> standing;
    /// Read without the mutex: while no gate stands, a request's start
    /// takes no lock.
    std::atomic<std::size_t> count = 0;
    ipc::BincodePeer* peer = nullptr;
};

WorkerGates worker_gates;

void erase_gate(std::vector<WorkerGate>::iterator it) {
    worker_gates.standing.erase(it);
    worker_gates.count.store(worker_gates.standing.size(), std::memory_order_relaxed);
}

/// Park the calling thread while a gate stands for `tag`.
void park_at_gate(llvm::StringRef tag) {
    auto& gates = worker_gates;
    if(gates.count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::unique_lock lock(gates.mutex);
    auto it = ranges::find_if(gates.standing, [&](const WorkerGate& gate) {
        return !gate.parked && gate.tag == tag;
    });
    if(it == gates.standing.end()) {
        return;
    }
    it->parked = true;
    auto id = it->id;
    it->relay.send([id] { worker_gates.peer->send_notification(worker::GateParkedParams{id}); });
    auto find = [id] {
        return ranges::find_if(worker_gates.standing,
                               [id](const WorkerGate& gate) { return gate.id == id; });
    };
    gates.changed.wait(lock, [&] { return find()->released; });
    erase_gate(find());
}

}  // namespace

llvm::StringRef build_kind_name(BuildKind kind) {
    return build_kind_names[std::to_underlying(kind)];
}

std::optional<BuildKind> parse_build_kind(llvm::StringRef name) {
    return parse_name<BuildKind>(build_kind_names, name);
}

std::optional<LoopEvent> parse_loop_event(llvm::StringRef name) {
    return parse_name<LoopEvent>(loop_event_names, name);
}

async::task<> TestHooks::returned(BuildKind kind, llvm::StringRef file) {
    builds[file][std::to_underlying(kind)] += 1;
    auto it = ranges::find_if(placed, [&](const Hold& hold) {
        return !hold.parked && hold.kind == kind && hold.file == file;
    });
    if(it == placed.end()) {
        co_return;
    }
    it->parked = true;
    // The release erases the hold before this frame resumes.
    auto released = it->released;
    on_held.emit(it->id);
    co_await released->wait();
}

std::uint64_t TestHooks::take_id() {
    auto id = next_id;
    next_id += 1;
    return id;
}

std::uint64_t TestHooks::hold(BuildKind kind, std::string file) {
    auto id = take_id();
    placed.push_back({.id = id, .kind = kind, .file = std::move(file)});
    return id;
}

std::uint64_t TestHooks::gate(std::string tag) {
    auto id = take_id();
    standing.push_back({.id = id, .tag = std::move(tag)});
    return id;
}

std::optional<std::uint64_t> TestHooks::sending(llvm::StringRef tag,
                                                const std::shared_ptr<ipc::BincodePeer>& peer) {
    auto it =
        ranges::find_if(standing, [&](const Gate& gate) { return !gate.sent && gate.tag == tag; });
    if(it == standing.end()) {
        return std::nullopt;
    }
    it->sent = true;
    it->worker = peer;
    std::erase_if(heard, [](const auto& worker) { return worker.expired(); });
    if(ranges::none_of(heard, [&](const auto& worker) { return worker.lock() == peer; })) {
        heard.push_back(peer);
        peer->on_notification(
            [this](const worker::GateParkedParams& params) { gate_parked(params.id); });
    }
    peer->send_notification(worker::GateParams{.id = it->id, .tag = it->tag});
    return it->id;
}

void TestHooks::gate_parked(std::uint64_t id) {
    auto it = ranges::find_if(standing, [&](const Gate& gate) { return gate.id == id; });
    if(it == standing.end()) {
        return;
    }
    it->parked = true;
    on_held.emit(id);
}

void TestHooks::answered(std::uint64_t id) {
    auto it = ranges::find_if(standing, [&](const Gate& gate) { return gate.id == id; });
    if(it != standing.end() && !it->parked) {
        release_gate(*it);
        standing.erase(it);
    }
}

bool TestHooks::release(std::uint64_t id) {
    if(auto it = ranges::find_if(standing, [&](const Gate& gate) { return gate.id == id; });
       it != standing.end()) {
        release_gate(*it);
        standing.erase(it);
        return true;
    }
    auto it = ranges::find_if(placed, [&](const Hold& hold) { return hold.id == id; });
    if(it == placed.end()) {
        return false;
    }
    it->released->set();
    placed.erase(it);
    return true;
}

void TestHooks::release_all() {
    for(auto& hold: placed) {
        hold.released->set();
    }
    placed.clear();
    for(auto& gate: standing) {
        release_gate(gate);
    }
    standing.clear();
}

std::uint64_t TestHooks::watch(LoopEvent event) {
    auto id = take_id();
    watches.push_back({.id = id, .event = event});
    return id;
}

void TestHooks::happened(LoopEvent event) {
    llvm::SmallVector<std::uint64_t> seen;
    std::erase_if(watches, [&](const Watch& watch) {
        if(watch.event != event) {
            return false;
        }
        seen.push_back(watch.id);
        return true;
    });
    for(auto id: seen) {
        on_held.emit(id);
    }
}

void install_test_hooks(ipc::BincodePeer& peer, async::event_loop& loop) {
    worker_gates.peer = &peer;
    peer.on_notification([&loop](const worker::GateParams& params) {
        auto& gates = worker_gates;
        std::lock_guard lock(gates.mutex);
        gates.standing.push_back(
            {.id = params.id, .tag = params.tag, .relay = loop.create_relay()});
        gates.count.store(gates.standing.size(), std::memory_order_relaxed);
    });
    peer.on_notification([](const worker::GateReleaseParams& params) {
        auto& gates = worker_gates;
        std::lock_guard lock(gates.mutex);
        auto it = ranges::find_if(gates.standing,
                                  [&](const WorkerGate& gate) { return gate.id == params.id; });
        assert(it != gates.standing.end());
        if(!it->parked) {
            erase_gate(it);
            return;
        }
        it->released = true;
        it->relay = {};
        gates.changed.notify_all();
    });
}

void run_test_hooks(llvm::StringRef tag) {
    park_at_gate(tag);
    this_thread::sleep_for(test_jitter());

    // The request whose tag contains CLICE_TEST_CRASH_REQUEST crashes its
    // worker right here — any kind of request, attributed like a real crash
    // of the kind CLICE_TEST_CRASH_KIND names, a trap by default.
    const static auto crash_request = env_string("CLICE_TEST_CRASH_REQUEST");
    if(crash_request && tag.contains(*crash_request)) {
        crash(env_string("CLICE_TEST_CRASH_KIND").value_or("trap"));
    }

    // The worker running the first request whose tag contains the text of
    // the file CLICE_TEST_KILL_REQUEST names dies as a kill from outside
    // would kill it: unannounced, the death names no request. Taking the
    // file away makes it the first — one rename of it succeeds, and a
    // respawned worker finds none.
    const static auto kill_request = env_string("CLICE_TEST_KILL_REQUEST");
    if(kill_request) {
        auto wanted = vfs::read_text(*kill_request);
        if(wanted && tag.contains(llvm::StringRef(*wanted).trim()) &&
           !fs::rename(*kill_request, *kill_request + ".taken")) {
#ifdef _WIN32
            std::_Exit(1);
#else
            ::raise(SIGKILL);
#endif
        }
    }
}

}  // namespace clice
