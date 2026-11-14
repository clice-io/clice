module clice;

import :command.database;
import :command.table;
import :command.toolchain;
import :compile.compilation;
import :project.graph.task_graph;
import :syntax.dependency_graph;
import :syntax.scan;
import :unittests.utils;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :vfs.file_table;

namespace clice::testing {
namespace {

using DispatchFn = std::function<async::task<Result>(std::uint32_t path_id, bool foreground)>;
using ResolveFn = std::function<llvm::SmallVector<std::uint32_t>(std::uint32_t path_id)>;

constexpr NodeKind test_kind = NodeKind{1};

NodeId nid(std::uint32_t path_id) {
    return {test_kind, path_id};
}

/// Legacy-shaped adapter driving the generalized TaskGraph, so the case
/// bodies keep the old CompileGraph vocabulary: one kind whose work waits
/// on the resolver's edges via depend() and then dispatches.
struct GraphShim {
    TaskGraph graph;
    DispatchFn dispatch;
    ResolveFn resolve;

    GraphShim(DispatchFn dispatch, ResolveFn resolve) :
        dispatch(std::move(dispatch)), resolve(std::move(resolve)) {}

    Work work() {
        return {.run = [this](Attempt& attempt) { return build(attempt); }};
    }

    async::task<Result> build(Attempt& attempt) {
        auto path_id = static_cast<std::uint32_t>(attempt.node().key);
        for(auto dep: resolve(path_id)) {
            switch(co_await attempt.depend(nid(dep), work())) {
                case Result::Success: break;
                case Result::Failed: co_return Result::Failed;
                case Result::Stale: co_return Result::Stale;
            }
        }
        co_return co_await dispatch(path_id, attempt.foreground());
    }

    async::task<bool> compile(std::uint32_t path_id, bool foreground = false) {
        auto result = co_await graph.join(nid(path_id),
                                          work(),
                                          {.foreground = foreground, .retry = [] { return true; }});
        co_return result == Result::Success;
    }

    llvm::SmallVector<std::uint32_t> update(std::uint32_t path_id) {
        llvm::SmallVector<std::uint32_t> dirtied;
        for(auto id: graph.invalidate(nid(path_id))) {
            dirtied.push_back(static_cast<std::uint32_t>(id.key));
        }
        return dirtied;
    }

    bool is_dirty(std::uint32_t path_id) const {
        return graph.is_dirty(nid(path_id));
    }

    bool is_compiling(std::uint32_t path_id) const {
        return graph.is_compiling(nid(path_id));
    }

    std::uint32_t refcount(std::uint32_t path_id) const {
        return graph.refcount(nid(path_id));
    }

    bool idle() const {
        return graph.idle();
    }

    async::task<> shutdown() {
        return graph.shutdown();
    }
};

/// Build a dispatch_fn that compiles PCMs in-process (no workers).
/// Clang requires ALL transitive PCM deps (not just direct imports)
/// in PrebuiltModuleFiles, so we pass every available PCM. Each build
/// writes a PCM of its own under `tmp`, as the master names every build's
/// PCM anew.
DispatchFn make_dispatch(CompilationDatabase& cdb,
                         Toolchain& toolchain,
                         DependencyGraph& graph,
                         const TempDir& tmp,
                         std::uint32_t& builds,
                         llvm::DenseMap<std::uint32_t, std::string>& pcm_paths) {
    return [&](std::uint32_t path_id, bool) -> async::task<Result> {
        auto file_path = cdb.files().resolve(Fid{path_id});
        auto candidates = cdb.candidate_entries(file_path);
        if(candidates.empty()) {
            co_return Result::Failed;
        }
        auto& entry = candidates.front();
        auto ref = database_command(cdb, entry);

        CompilationParams cp;
        cp.kind = CompilationKind::ModuleInterface;
        cp.input.directory = cdb.commands().get(entry.command).directory;
        auto arguments = toolchain.render(ref, cdb.files().spelling(ref.file)).argv;
        cp.input.arguments.assign(arguments.begin(), arguments.end());

        // Fill ALL available PCM paths (clang needs transitive deps too).
        for(auto& [pid, pcm_path]: pcm_paths) {
            for(auto& [mod_name, mod_ids]: graph.modules()) {
                if(ranges::find(mod_ids, Fid{pid}) != mod_ids.end()) {
                    cp.input.pcms.try_emplace(mod_name, pcm_path);
                    break;
                }
            }
        }

        builds += 1;
        auto pcm = tmp.path(std::format("{}.pcm", builds));
        cp.output_file = pcm;

        auto unit = compile(std::move(cp));

        if(unit->has_output()) {
            pcm_paths[path_id] = pcm;
            co_return Result::Success;
        }
        co_return Result::Failed;
    };
}

/// Build a resolve_fn that lazily scans module files for imports.
ResolveFn make_resolver(CompilationDatabase& cdb, Toolchain& toolchain, DependencyGraph& graph) {
    return [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = cdb.files().resolve(Fid{path_id});
        auto candidates = cdb.candidate_entries(file_path);
        if(candidates.empty()) {
            return {};
        }
        auto& entry = candidates.front();
        auto ref = database_command(cdb, entry);

        auto scan_result = scan_precise(toolchain.render(ref, cdb.files().spelling(ref.file)).argv,
                                        cdb.commands().get(entry.command).directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0].raw);
            }
        }
        return deps;
    };
}

/// Helper to set up infra, compile a module, and verify all PCMs are produced.
struct ModuleTestEnv {
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
    Toolchain toolchain{commands, file_table};
    DependencyGraph graph;
    std::uint32_t pcm_builds = 0;
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    /// List `entries` in the database and scan each under its own command.
    void setup(llvm::ArrayRef<CDBEntry> entries) {
        write_cdb(tmp, cdb, build_cdb_json(entries));
        scan_all(cdb, toolchain, graph);
    }

    std::uint32_t lookup(llvm::StringRef mod_name) {
        auto ids = graph.lookup_module(mod_name);
        return ids.empty() ? UINT32_MAX : ids[0].raw;
    }
};

ZEST_SUITE(PCMGraphIntegration, zest::LoopFixture) {

ModuleTestEnv env;
std::optional<GraphShim> cg;

DispatchFn default_dispatch() {
    return make_dispatch(env.cdb, env.toolchain, env.graph, env.tmp, env.pcm_builds, env.pcm_paths);
}

ResolveFn default_resolver() {
    return make_resolver(env.cdb, env.toolchain, env.graph);
}

void make_graph(DispatchFn dispatch, ResolveFn resolve) {
    cg.emplace(std::move(dispatch), std::move(resolve));
}

void make_graph() {
    make_graph(default_dispatch(), default_resolver());
}

/// Run the test body, then verify the shutdown protocol leaves the graph idle.
template <typename F>
void execute(F&& fn) {
    // Hang detection: the modules compile for real, slowly on a loaded
    // machine.
    watchdog = chrono::seconds(45);
    auto wrapper = [&]() -> async::task<> {
        co_await fn();
        co_await cg->shutdown();
        ZEXPECT(cg->idle());
    };
    run(wrapper());
}

/// ============================================================================
///              Recompile after update (invalidation + recompile)
/// ============================================================================

ZEST_CASE(RecompileAfterUpdate) {
    env.tmp.touch("leaf.cppm",
                  "export module Leaf;\n"
                  "export int leaf() { return 1; }\n");
    env.tmp.touch("mid.cppm",
                  "export module Mid;\n"
                  "import Leaf;\n"
                  "export int mid() { return leaf() + 1; }\n");

    env.setup({
        {env.tmp.root, env.tmp.path("leaf.cppm"), {}},
        {env.tmp.root, env.tmp.path("mid.cppm"),  {}},
    });

    auto pid_leaf = env.lookup("Leaf");
    auto pid_mid = env.lookup("Mid");
    ZASSERT(pid_leaf != UINT32_MAX);
    ZASSERT(pid_mid != UINT32_MAX);

    make_graph();
    execute([&]() -> async::task<> {
        // First compile.
        auto r1 = co_await cg->compile(pid_mid).catch_cancel();
        ZEXPECT((r1.has_value() && *r1));
        ZEXPECT(env.pcm_paths.size() == 2u);
        ZEXPECT(!cg->is_dirty(pid_leaf));
        ZEXPECT(!cg->is_dirty(pid_mid));

        // Simulate editing Leaf — should cascade to Mid.
        cg->update(pid_leaf);
        ZEXPECT(cg->is_dirty(pid_leaf));
        ZEXPECT(cg->is_dirty(pid_mid));

        // Recompile.
        auto r2 = co_await cg->compile(pid_mid).catch_cancel();
        ZEXPECT((r2.has_value() && *r2));
        ZEXPECT(!cg->is_dirty(pid_leaf));
        ZEXPECT(!cg->is_dirty(pid_mid));
    });
}

/// ============================================================================
///                      Diamond update cascade + recompile
/// ============================================================================

ZEST_CASE(DiamondUpdateCascade) {
    env.tmp.touch("mod_base.cppm",
                  "export module Base;\n"
                  "export int base_val() { return 10; }\n");
    env.tmp.touch("mod_left.cppm",
                  "export module Left;\n"
                  "import Base;\n"
                  "export int left_val() { return base_val() + 1; }\n");
    env.tmp.touch("mod_right.cppm",
                  "export module Right;\n"
                  "import Base;\n"
                  "export int right_val() { return base_val() + 2; }\n");
    env.tmp.touch("mod_top.cppm",
                  "export module Top;\n"
                  "import Left;\n"
                  "import Right;\n"
                  "export int top_val() { return left_val() + right_val(); }\n");

    env.setup({
        {env.tmp.root, env.tmp.path("mod_base.cppm"),  {}},
        {env.tmp.root, env.tmp.path("mod_left.cppm"),  {}},
        {env.tmp.root, env.tmp.path("mod_right.cppm"), {}},
        {env.tmp.root, env.tmp.path("mod_top.cppm"),   {}},
    });

    auto pid_base = env.lookup("Base");
    auto pid_left = env.lookup("Left");
    auto pid_right = env.lookup("Right");
    auto pid_top = env.lookup("Top");
    ZASSERT(pid_base != UINT32_MAX);
    ZASSERT(pid_top != UINT32_MAX);

    make_graph();
    execute([&]() -> async::task<> {
        // Initial compile.
        auto r1 = co_await cg->compile(pid_top).catch_cancel();
        ZEXPECT((r1.has_value() && *r1));
        ZEXPECT(env.pcm_paths.size() == 4u);

        // Save old PCM paths.
        auto old_base_pcm = env.pcm_paths[pid_base];

        // Update base: should cascade to Left, Right, Top.
        auto dirtied = cg->update(pid_base);
        ZEXPECT(cg->is_dirty(pid_base));
        ZEXPECT(cg->is_dirty(pid_left));
        ZEXPECT(cg->is_dirty(pid_right));
        ZEXPECT(cg->is_dirty(pid_top));

        // Simulate MasterServer: erase stale PCMs for all dirtied nodes.
        for(auto id: dirtied) {
            env.pcm_paths.erase(id);
        }
        ZEXPECT(env.pcm_paths.size() == 0u);

        // Recompile.
        auto r2 = co_await cg->compile(pid_top).catch_cancel();
        ZEXPECT((r2.has_value() && *r2));
        ZEXPECT(env.pcm_paths.size() == 4u);
        // PCM path should have changed (new temp file).
        ZEXPECT(env.pcm_paths[pid_base] != old_base_pcm);
    });
}

/// ============================================================================
///        Verify resolve_fn is re-invoked after update (resolved=false)
/// ============================================================================

ZEST_CASE(ResolveAgainAfterUpdate) {
    // Start with Mid importing Leaf.
    env.tmp.touch("leaf.cppm",
                  "export module Leaf;\n"
                  "export int leaf() { return 1; }\n");
    env.tmp.touch("extra.cppm",
                  "export module Extra;\n"
                  "export int extra() { return 99; }\n");
    env.tmp.touch("mid.cppm",
                  "export module Mid;\n"
                  "import Leaf;\n"
                  "export int mid() { return leaf() + 1; }\n");

    env.setup({
        {env.tmp.root, env.tmp.path("leaf.cppm"),  {}},
        {env.tmp.root, env.tmp.path("extra.cppm"), {}},
        {env.tmp.root, env.tmp.path("mid.cppm"),   {}},
    });

    auto pid_leaf = env.lookup("Leaf");
    auto pid_extra = env.lookup("Extra");
    auto pid_mid = env.lookup("Mid");
    ZASSERT(pid_mid != UINT32_MAX);
    ZASSERT(pid_extra != UINT32_MAX);

    int resolve_count = 0;
    auto counting_resolver =
        [&, inner = default_resolver()](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == pid_mid) {
            resolve_count++;
        }
        return inner(path_id);
    };

    make_graph(default_dispatch(), std::move(counting_resolver));
    execute([&]() -> async::task<> {
        // First compile: resolve_fn called once for Mid.
        auto r1 = co_await cg->compile(pid_mid).catch_cancel();
        ZEXPECT((r1.has_value() && *r1));
        ZEXPECT(resolve_count == 1);

        // Update Mid: resets resolved.
        cg->update(pid_mid);

        // Recompile: resolve_fn should be called again.
        auto r2 = co_await cg->compile(pid_mid).catch_cancel();
        ZEXPECT((r2.has_value() && *r2));
        ZEXPECT(resolve_count == 2);
    });
}

/// ============================================================================
///              Compilation failure propagation (real clang error)
/// ============================================================================

ZEST_CASE(CompileFailurePropagates) {
    // Good module.
    env.tmp.touch("good.cppm",
                  "export module Good;\n"
                  "export int good() { return 1; }\n");
    // Bad module with syntax error.
    env.tmp.touch("bad.cppm",
                  "export module Bad;\n"
                  "import Good;\n"
                  "export int bad() { return UNDEFINED_SYMBOL; }\n");

    env.setup({
        {env.tmp.root, env.tmp.path("good.cppm"), {}},
        {env.tmp.root, env.tmp.path("bad.cppm"),  {}},
    });

    auto pid_bad = env.lookup("Bad");
    ZASSERT(pid_bad != UINT32_MAX);

    make_graph();
    execute([&]() -> async::task<> {
        auto result = co_await cg->compile(pid_bad).catch_cancel();
        ZEXPECT(result);
        // Compilation should fail due to undefined symbol.
        ZEXPECT(!*result);
        // Good module should still have been compiled successfully.
        auto pid_good = env.lookup("Good");
        ZEXPECT(env.pcm_paths.contains(pid_good));
        // Bad module should NOT have a PCM.
        ZEXPECT(!env.pcm_paths.contains(pid_bad));
    });
}

/// ============================================================================
///    Shared dependency: switching import target must not kill or restart it
/// ============================================================================

ZEST_CASE(SharedDependencyImportSwitch) {
    env.tmp.touch("shared.cppm",
                  "export module Shared;\n"
                  "export int shared_val() { return 1; }\n");
    env.tmp.touch("a.cppm",
                  "export module A;\n"
                  "import Shared;\n"
                  "export int a_val() { return shared_val(); }\n");
    env.tmp.touch("c.cppm",
                  "export module C;\n"
                  "import Shared;\n"
                  "export int c_val() { return shared_val(); }\n");

    env.setup({
        {env.tmp.root, env.tmp.path("shared.cppm"), {}},
        {env.tmp.root, env.tmp.path("a.cppm"),      {}},
        {env.tmp.root, env.tmp.path("c.cppm"),      {}},
    });

    auto pid_shared = env.lookup("Shared");
    auto pid_a = env.lookup("A");
    auto pid_c = env.lookup("C");
    ZASSERT(pid_shared != UINT32_MAX);
    ZASSERT(pid_a != UINT32_MAX);
    ZASSERT(pid_c != UINT32_MAX);

    // Gate the shared module's dispatch so the import switch can be injected
    // while it is still compiling.
    async::event shared_started;
    async::event shared_proceed;
    int shared_calls = 0;
    auto inner = default_dispatch();
    auto dispatch = [&](std::uint32_t pid, bool) -> async::task<Result> {
        if(pid == pid_shared) {
            shared_calls += 1;
            shared_started.set();
            co_await shared_proceed.wait();
        }
        co_return co_await inner(pid, false);
    };

    make_graph(std::move(dispatch), default_resolver());

    async::cancellation_source req_a;
    async::event start_c;
    std::optional<bool> result_a, result_c;

    auto request_a = [&]() -> async::task<> {
        auto r = co_await async::with_token(cg->compile(pid_a), req_a.token());
        if(r.has_value()) {
            result_a = *r;
        }
    };

    auto request_c = [&]() -> async::task<> {
        co_await start_c.wait();
        auto r = co_await cg->compile(pid_c).catch_cancel();
        if(r.has_value()) {
            result_c = *r;
        }
    };

    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await shared_started.wait();
            ZEXPECT(cg->refcount(pid_shared) == 1u);
            ZEXPECT(shared_calls == 1);

            // Simulate switching `import A` to `import C`: start the new
            // request first, then cancel the old one — the same order the
            // edit path of ServedProject::ensure_compiled uses, so the
            // shared module's interest never drops to zero.
            start_c.set();
            req_a.cancel();
            co_await cg->graph.attempt_finished(nid(pid_a));

            ZEXPECT(cg->is_compiling(pid_shared));
            ZEXPECT(cg->refcount(pid_shared) == 1u);
            ZEXPECT(shared_calls == 1);

            shared_proceed.set();
            co_return;
        };

        co_await async::when_all(request_a(), request_c(), driver());

        // A was cancelled; C completed using the surviving shared build.
        ZEXPECT(!result_a.has_value());
        ZEXPECT(result_c == true);
        ZEXPECT(shared_calls == 1);
        ZEXPECT(env.pcm_paths.contains(pid_shared));
        ZEXPECT(env.pcm_paths.contains(pid_c));
        ZEXPECT(!env.pcm_paths.contains(pid_a));
    });
}

/// ============================================================================
///   Shared dependency failure propagates to every consumer of the same attempt
/// ============================================================================

ZEST_CASE(SharedDependencyFailsBoth) {
    env.tmp.touch("shared.cppm",
                  "export module Shared;\n"
                  "export int shared_val() { return UNDEFINED_SYMBOL; }\n");
    env.tmp.touch("a.cppm",
                  "export module A;\n"
                  "import Shared;\n"
                  "export int a_val() { return shared_val(); }\n");
    env.tmp.touch("c.cppm",
                  "export module C;\n"
                  "import Shared;\n"
                  "export int c_val() { return shared_val(); }\n");

    env.setup({
        {env.tmp.root, env.tmp.path("shared.cppm"), {}},
        {env.tmp.root, env.tmp.path("a.cppm"),      {}},
        {env.tmp.root, env.tmp.path("c.cppm"),      {}},
    });

    auto pid_shared = env.lookup("Shared");
    auto pid_a = env.lookup("A");
    auto pid_c = env.lookup("C");
    ZASSERT(pid_shared != UINT32_MAX);

    // Gate the shared module so both consumers join the same failing attempt.
    async::event shared_started;
    async::event shared_proceed;
    int shared_calls = 0;
    auto inner = default_dispatch();
    auto dispatch = [&](std::uint32_t pid, bool) -> async::task<Result> {
        if(pid == pid_shared) {
            shared_calls += 1;
            shared_started.set();
            co_await shared_proceed.wait();
        }
        co_return co_await inner(pid, false);
    };

    // C's attempt waits on the shared module right after resolving its
    // imports, before anything else runs.
    async::event c_resolved;
    auto resolver = [&, inner = default_resolver()](std::uint32_t pid) {
        auto imports = inner(pid);
        if(pid == pid_c) {
            c_resolved.set();
        }
        return imports;
    };
    make_graph(std::move(dispatch), std::move(resolver));

    std::optional<bool> result_a, result_c;
    auto request_a = [&]() -> async::task<> {
        auto r = co_await cg->compile(pid_a).catch_cancel();
        if(r.has_value()) {
            result_a = *r;
        }
    };
    auto request_c = [&]() -> async::task<> {
        auto r = co_await cg->compile(pid_c).catch_cancel();
        if(r.has_value()) {
            result_c = *r;
        }
    };

    execute([&]() -> async::task<> {
        auto driver = [&]() -> async::task<> {
            co_await shared_started.wait();
            co_await c_resolved.wait();
            // Both chains hold interest in the same attempt.
            ZEXPECT(cg->refcount(pid_shared) == 2u);
            shared_proceed.set();
            co_return;
        };

        co_await async::when_all(request_a(), request_c(), driver());

        // One failing attempt, both consumers fail without retry.
        ZEXPECT(result_a == false);
        ZEXPECT(result_c == false);
        ZEXPECT(shared_calls == 1);
        ZEXPECT(!env.pcm_paths.contains(pid_shared));
        ZEXPECT(!env.pcm_paths.contains(pid_a));
        ZEXPECT(!env.pcm_paths.contains(pid_c));
    });
}

};  // ZEST_SUITE(PCMGraphIntegration)

}  // namespace
}  // namespace clice::testing
