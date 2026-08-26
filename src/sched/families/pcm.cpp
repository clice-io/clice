#include "sched/families/pcm.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "sched/families/build_common.h"
#include "support/anomaly.h"
#include "support/logging.h"
#include "syntax/scan.h"
#include "worker/protocol.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/xxhash.h"
#include "clang/Basic/Version.h"

namespace clice {

PCMFamily::PCMFamily(TaskGraph& graph,
                     Workspace& workspace,
                     ContextResolver& contexts,
                     WorkerPool& pool) :
    graph(graph), workspace(workspace), contexts(contexts), pool(pool) {}

void PCMFamily::register_runner() {
    graph.register_family(pcm_family, [this](RoundContext& ctx, NodeId id) {
        return run(ctx, static_cast<std::uint32_t>(id.key));
    });
}

PCMFamily::ModuleDeps PCMFamily::direct_deps(std::uint32_t path_id,
                                             std::optional<llvm::StringRef> content) {
    auto file_path = workspace.path_pool.resolve(path_id);
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(file_path, rule_append, rule_remove);
    auto results = workspace.cdb.lookup(file_path, {.remove = rule_remove, .append = rule_append});
    if(results.empty())
        return {};
    workspace.toolchain.resolve_or_warn(results[0]);

    auto& cmd = results[0];
    return direct_deps(path_id, cmd.to_argv(), cmd.resolved.directory, content);
}

PCMFamily::ModuleDeps PCMFamily::direct_deps(std::uint32_t path_id,
                                             llvm::ArrayRef<const char*> arguments,
                                             llvm::StringRef directory,
                                             std::optional<llvm::StringRef> content) {
    auto scan_result = scan_precise(arguments, directory, content);

    // Every scanned name lands in the edge set, resolved or not: an
    // unresolved name edges to its sentinel, which is what lets the
    // name's first provider re-dirty this unit through the ordinary
    // cascade later — no side bookkeeping of who failed against it.
    ModuleDeps deps;
    auto add = [&](llvm::StringRef name) {
        auto mod_ids = workspace.dep_graph.lookup_module(name);
        if(mod_ids.empty()) {
            deps.declared.push_back(unresolved_node(name));
        } else {
            deps.resolved.push_back(mod_ids[0]);
            deps.declared.push_back(node(mod_ids[0]));
        }
    };

    for(auto& mod_name: scan_result.modules) {
        add(mod_name);
    }

    // Module implementation units implicitly depend on their interface unit.
    if(!scan_result.module_name.empty() && !scan_result.is_interface_unit) {
        add(scan_result.module_name);
    }

    return deps;
}

llvm::SmallVector<NodeId> PCMFamily::provider_appeared(llvm::StringRef name) {
    auto dirtied = graph.update(unresolved_node(name));
    for(auto id: dirtied) {
        // Dirtied module units drop their cached PCM state, exactly as
        // invalidate() does for content changes — a PCM built against
        // the unresolved name embeds the failure.
        if(id.family == pcm_family && !is_unresolved(id)) {
            auto pid = static_cast<std::uint32_t>(id.key);
            workspace.pcm_paths.erase(pid);
            workspace.pcm_cache.erase(pid);
        }
    }
    return dirtied;
}

void PCMFamily::declare_deps(std::uint32_t path_id, llvm::ArrayRef<NodeId> deps) {
    graph.declare(node(path_id), deps);
}

kota::task<RoundOutcome> PCMFamily::run(RoundContext& ctx, std::uint32_t path_id) {
    // The import list is scanner truth, not build output: commit it as
    // durable edges before building, so a unit whose build fails stays
    // cascade-reachable from its imports — fixing an import must re-dirty
    // the units it broke. The lazy per-round resolve is what keeps the
    // edges honest: a CDB or import change is always seen by the next
    // round. Each depend() then records the candidate edge (interest- and
    // foreground-visible immediately) and waits for the dependency.
    auto deps = direct_deps(path_id);
    declare_deps(path_id, deps.declared);
    for(auto dep: deps.declared) {
        if(is_unresolved(dep)) {
            ctx.reference(dep);
        }
    }
    for(auto dep: deps.resolved) {
        switch(co_await ctx.depend(node(dep))) {
            case DependResult::Ready: break;
            case DependResult::Failed: co_return RoundOutcome::Failed;
            case DependResult::Cancelled: co_return RoundOutcome::Stale;
        }
    }

    auto mod_it = workspace.path_to_module.find(path_id);
    if(mod_it == workspace.path_to_module.end())
        co_return RoundOutcome::Failed;

    // Copy out of the map before any suspension below: while a PCM build
    // is awaited, a concurrent didSave can insert into (or erase from)
    // path_to_module, rehashing the DenseMap and invalidating mod_it.
    auto module_name = mod_it->second;

    auto file_path = std::string(workspace.path_pool.resolve(path_id));

    worker::BuildPCMParams bp;
    bp.file = file_path;
    contexts.resolve_command(file_path, bp.directory, bp.arguments);

    if(!workspace.store) {
        LOG_WARN("BuildPCM skipped for module {}: cache store is unavailable", module_name);
        co_return RoundOutcome::Failed;
    }

    // Deterministic content-addressed PCM key over the source path and
    // the frontend-relevant subset of the compile flags.
    auto safe_module_name = module_name;
    std::ranges::replace(safe_module_name, ':', '-');
    auto pcm_key = std::format("{}-{}",
                               safe_module_name,
                               cache_key({clang::getClangFullVersion(),
                                          bp.directory,
                                          file_path,
                                          canonicalize(bp.arguments, ArgsProfile::Frontend)}));

    // Check if cached PCM is still valid.
    llvm::StringRef pcm_miss = "no_entry";
    if(auto pcm_it = workspace.pcm_cache.find(path_id); pcm_it != workspace.pcm_cache.end()) {
        if(pcm_it->second.key != pcm_key) {
            pcm_miss = "key_changed";
        } else if(!workspace.store->lookup("pcm", pcm_key)) {
            pcm_miss = "evicted";
        } else if(deps_changed(workspace.path_pool, pcm_it->second.deps)) {
            pcm_miss = "deps_changed";
        } else {
            workspace.pcm_paths[path_id] = pcm_it->second.path;
            LOG_PERF("cache", "ns=pcm event=hit key={} module={}", pcm_key, module_name);
            co_return RoundOutcome::Success;
        }
    }
    LOG_PERF("cache",
             "ns=pcm event=miss reason={} key={} module={}",
             pcm_miss,
             pcm_key,
             module_name);

    // Same shared-artifact budget as the PCH, but keyed with the
    // module's current content: unlike pch_key (which embeds the
    // preamble text), pcm_key is content-free, and a blocked budget
    // must unlock the moment the poison is edited.
    auto content = llvm::MemoryBuffer::getFile(file_path);
    auto budget_key = std::format("{}-{:016x}",
                                  pcm_key,
                                  content ? llvm::xxh3_64bits((*content)->getBuffer()) : 0);
    if(workspace.build_crashes.blocked(budget_key)) {
        LOG_WARN("PCM build for module {} refused: key {} keeps crashing workers",
                 module_name,
                 budget_key);
        co_return RoundOutcome::Failed;
    }

    bp.module_name = module_name;
    auto pending = workspace.store->begin_store("pcm", pcm_key);
    bp.output_path = pending.tmp_path;

    // Clang needs ALL transitive PCM deps, not just direct imports.
    // Exclude the module being built — its old PCM path may still be
    // in pcm_paths from a previous (now-invalidated) build.
    workspace.fill_pcm_deps(bp.pcms, path_id);

    // The interest class is read at dispatch time: a foreground requester
    // may have joined after this round started. The advisory token rides
    // into the pool, which translates a fire into the cooperative
    // CancelBuild while this frame keeps awaiting the real reply
    // (contract 2 — the slot frees only when the worker is truly idle).
    auto priority = ctx.foreground() ? worker::Priority::High : worker::Priority::Low;
    auto result = co_await pool.send_stateless(bp, priority, {}, ctx.token());
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        workspace.build_crashes.on_crash(budget_key);
        result = co_await pool.send_stateless(bp, priority, {}, ctx.token());
        if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
            workspace.build_crashes.on_crash(budget_key);
        }
    }

    // A scheduler preemption (foreground reclaim, memory pressure) or an
    // advisory cancel is no verdict on the unit: report the round stale so
    // waiters drive a retry instead of failing their whole chain.
    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("BuildPCM preempted for module {}, will retry", module_name);
        co_return RoundOutcome::Stale;
    }
    if(!result.has_value() || !result.value().success) {
        if(expected_build_failure(result)) {
            LOG_WARN("BuildPCM failed for module {}: {}",
                     module_name,
                     build_failure_message(result));
        } else {
            LOG_ANOMALY(PCMBuildFail,
                        "PCM build failed for module {}: {}",
                        module_name,
                        build_failure_message(result));
        }
        co_return RoundOutcome::Failed;
    }

    // Commit on the thread pool: it fsyncs the freshly written PCM.
    auto committed =
        co_await kota::queue([&] { return workspace.store->commit(std::move(pending)); });
    if(!committed.has_value() || !committed.value().has_value()) {
        LOG_WARN("Failed to commit PCM for module {}", module_name);
        co_return RoundOutcome::Failed;
    }

    workspace.build_crashes.on_land(budget_key);
    auto pcm_path = std::move(committed.value().value());
    workspace.pcm_paths[path_id] = pcm_path;
    workspace.pcm_cache[path_id] = {
        pcm_path,
        pcm_key,
        capture_deps_snapshot(workspace.path_pool, result.value().deps, result.value().build_at)};
    LOG_INFO("Built PCM for module {}: {}", module_name, pcm_path);

    // Persist cache metadata after successful build.
    workspace.save_cache(contexts);

    // Signal that new index data is available for background merge.
    if(on_indexing_needed)
        on_indexing_needed();

    co_return RoundOutcome::Success;
}

bool PCMFamily::revalidate_blobs() {
    llvm::SmallVector<std::uint32_t> evicted;
    for(auto& [pid, pcm_path]: workspace.pcm_paths) {
        if(!llvm::sys::fs::exists(pcm_path)) {
            evicted.push_back(pid);
        }
    }
    for(auto pid: evicted) {
        // Artifact tier only: the blob vanished, its content did not, so
        // importers' landed results stay valid. A full invalidate() here
        // would void the very consumer round that is revalidating before
        // rebuilding its imports — under a cache budget smaller than the
        // working set, that voids and respawns the waiter forever.
        graph.mark_dirty(node(pid));
        workspace.pcm_paths.erase(pid);
        workspace.pcm_cache.erase(pid);
    }
    return !evicted.empty();
}

kota::task<bool> PCMFamily::prepare_deps(std::uint32_t path_id,
                                         llvm::ArrayRef<const char*> arguments,
                                         llvm::StringRef directory,
                                         std::optional<llvm::StringRef> content,
                                         bool foreground) {
    // A project without module units pays nothing. A CDB reload that
    // introduces modules mid-session takes effect on the next call.
    if(workspace.path_to_module.empty()) {
        co_return true;
    }

    // Resolved fresh on every call — a stale list must never outlive a
    // CDB change. The requester never runs a round here, but its
    // consumer edges must live in the graph: a saved module (or a
    // provider appearing for a sentinel) cascades to the open TUs
    // importing it through them. Declared even when empty, so a removed
    // import stops cascading.
    auto deps = direct_deps(path_id, arguments, directory, content);
    // A module unit's PCM node carries its ARTIFACT's edge truth, owned
    // by its own rounds — a request's buffer view must not overwrite it
    // (an unsaved removed import would disconnect the cached PCM from
    // the very dependency whose save should invalidate it). Plain TUs'
    // consumer nodes never run rounds; the declaration is theirs alone.
    if(!workspace.path_to_module.contains(path_id)) {
        declare_deps(path_id, deps.declared);
    }
    if(deps.resolved.empty()) {
        co_return true;
    }

    for(int attempt = 0; attempt < 3; ++attempt) {
        bool any_evicted = revalidate_blobs();
        if(attempt > 0 && !any_evicted) {
            break;
        }

        std::vector<kota::task<JoinOutcome>> waits;
        waits.reserve(deps.resolved.size());
        for(auto dep: deps.resolved) {
            waits.push_back(graph.request(node(dep), {.foreground = foreground}));
        }
        auto results = co_await kota::when_all(std::move(waits));
        bool ok = std::ranges::all_of(results, [](JoinOutcome outcome) {
            return outcome == JoinOutcome::Success;
        });
        if(!ok) {
            co_return false;
        }
    }
    co_return true;
}

bool PCMFamily::tracks(std::uint32_t path_id) const {
    return graph.has_node(node(path_id));
}

llvm::SmallVector<std::uint32_t> PCMFamily::invalidate(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> dirtied;
    for(auto id: graph.update(node(path_id))) {
        auto pid = static_cast<std::uint32_t>(id.key);
        workspace.pcm_paths.erase(pid);
        workspace.pcm_cache.erase(pid);
        dirtied.push_back(pid);
    }
    return dirtied;
}

}  // namespace clice
