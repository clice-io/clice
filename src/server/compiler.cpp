#include "server/compiler.h"

#include <format>
#include <ranges>
#include <string>

#include "command/search_config.h"
#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/serde/json/json.h"
#include "index/tu_index.h"
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace lsp = eventide::ipc::lsp;
using serde_raw = et::serde::RawValue;

/// Detect whether the cursor is inside a preamble directive (include/import).

Compiler::Compiler(et::event_loop& loop,
                   et::ipc::JsonPeer& peer,
                   Workspace& workspace,
                   WorkerPool& pool,
                   llvm::DenseMap<std::uint32_t, Session>& sessions) :
    loop(loop), peer(peer), workspace(workspace), pool(pool), sessions(sessions) {}

Compiler::~Compiler() {
    workspace.cancel_all();
}

void Compiler::init_compile_graph() {
    if(workspace.path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        return;
    }

    // Lazy dependency resolver: scans a module file on demand to discover imports.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = workspace.path_pool.resolve(path_id);
        auto results =
            workspace.cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty())
            return {};

        auto& cmd = results[0];
        auto scan_result = scan_precise(cmd.to_argv(), cmd.resolved.directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = workspace.dep_graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        // Module implementation units implicitly depend on their interface unit.
        if(!scan_result.module_name.empty() && !scan_result.is_interface_unit) {
            auto mod_ids = workspace.dep_graph.lookup_module(scan_result.module_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        return deps;
    };

    // Dispatch: sends BuildPCM request to a stateless worker.
    auto dispatch = [this](std::uint32_t path_id) -> et::task<bool> {
        auto mod_it = workspace.path_to_module.find(path_id);
        if(mod_it == workspace.path_to_module.end())
            co_return false;

        auto file_path = std::string(workspace.path_pool.resolve(path_id));

        auto cmd_opt = resolve_compile_command(file_path);
        if(!cmd_opt)
            co_return false;
        auto& cmd = *cmd_opt;

        worker::BuildParams bp;
        bp.kind = worker::BuildKind::BuildPCM;
        bp.file = file_path;
        bp.directory = cmd.resolved.directory.str();
        bp.arguments = cmd.to_string_argv();

        // Content-addressed key: file path + compile flags (source-file-free).
        auto artifact_key = ArtifactCache::compute_key_with_path(file_path, cmd.resolved.flags);

        // Deterministic PCM output path.
        auto safe_module_name = mod_it->second;
        std::ranges::replace(safe_module_name, ':', '-');
        auto pcm_filename = std::format("{}-{:016x}.pcm", safe_module_name, artifact_key);
        auto pcm_path = path::join(workspace.config.cache_dir, "cache", "pcm", pcm_filename);

        // Check if cached artifact is still valid.
        if(auto* cached = workspace.artifact_cache.lookup(artifact_key)) {
            if(!cached->path.empty() && llvm::sys::fs::exists(cached->path) &&
               !deps_changed(workspace.path_pool, cached->deps)) {
                workspace.pcm_active[path_id] = artifact_key;
                co_return true;
            }
        }

        bp.module_name = mod_it->second;
        bp.output_path = pcm_path;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        workspace.fill_pcm_deps(bp.pcms);

        auto result = co_await pool.send_stateless(bp);
        if(!result.has_value() || !result.value().success) {
            LOG_WARN("BuildPCM failed for module {}: {}",
                     mod_it->second,
                     result.has_value() ? result.value().error : result.error().message);
            co_return false;
        }

        // Collect input artifact keys from dependent PCMs.
        // By the time dispatch runs, all dependencies are already compiled
        // (CompileGraph guarantees topological order).
        llvm::SmallVector<ArtifactKey> input_keys;
        for(auto& [mod_name, pcm_dep_path]: bp.pcms) {
            // Find which path_id owns this PCM path and get its artifact key.
            for(auto& [dep_pid, dep_key]: workspace.pcm_active) {
                auto* dep_entry = workspace.artifact_cache.lookup(dep_key);
                if(dep_entry && dep_entry->path == pcm_dep_path) {
                    input_keys.push_back(dep_key);
                    break;
                }
            }
        }

        // Insert into artifact cache.
        ArtifactEntry artifact;
        artifact.path = result.value().output_path;
        artifact.inputs.assign(input_keys.begin(), input_keys.end());
        artifact.deps = capture_deps_snapshot(workspace.path_pool, result.value().deps);
        workspace.artifact_cache.insert(artifact_key, std::move(artifact));

        // Update active state.
        workspace.pcm_active[path_id] = artifact_key;
        LOG_INFO("Built PCM for module {}: {}", mod_it->second, result.value().output_path);

        // Persist cache metadata after successful build.
        workspace.save_cache();

        // Signal that new index data is available for background merge.
        if(on_indexing_needed)
            on_indexing_needed();

        co_return true;
    };

    workspace.compile_graph =
        std::make_unique<CompileGraph>(std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", workspace.path_to_module.size());
}

std::optional<CompileCommand> Compiler::resolve_compile_command(llvm::StringRef path,
                                                                Session* session) {
    auto path_id = workspace.path_pool.intern(path);

    // 1. If the session has an active header context via switchContext,
    //    use the host source's CDB entry with file path replaced and preamble injected.
    if(session && session->active_context.has_value()) {
        return resolve_header_context_command(path, path_id, session);
    }

    // 2. Normal CDB lookup for the file itself.
    auto results = workspace.cdb.lookup(path, {.query_toolchain = true});
    if(!results.empty()) {
        return std::move(results.front());
    }

    // 3. No CDB entry — try automatic header context resolution.
    return resolve_header_context_command(path, path_id, session);
}

std::optional<CompileCommand> Compiler::resolve_header_context_command(llvm::StringRef path,
                                                                       std::uint32_t path_id,
                                                                       Session* session) {
    // Use cached context if available; otherwise resolve.
    // If an active context override exists, invalidate cache if it points to
    // a different host so we re-resolve with the correct one.
    const HeaderFileContext* ctx_ptr = nullptr;
    if(session && session->header_context.has_value()) {
        if(session->active_context.has_value() &&
           session->header_context->host_path_id != *session->active_context) {
            session->header_context.reset();
        } else {
            ctx_ptr = &*session->header_context;
        }
    }
    if(!ctx_ptr) {
        auto resolved = resolve_header_context(path_id, session);
        if(!resolved) {
            LOG_WARN("No CDB entry and no header context for {}", path);
            return std::nullopt;
        }
        if(session) {
            session->header_context = std::move(*resolved);
            ctx_ptr = &*session->header_context;
        } else {
            // Background indexing path — no session to store on.
            static thread_local std::optional<HeaderFileContext> tl_ctx;
            tl_ctx = std::move(*resolved);
            ctx_ptr = &*tl_ctx;
        }
    }

    auto host_path = workspace.path_pool.resolve(ctx_ptr->host_path_id);
    auto host_results = workspace.cdb.lookup(host_path, {.query_toolchain = true});
    if(host_results.empty()) {
        LOG_WARN("resolve_header_context_command: host {} has no CDB entry", host_path);
        return std::nullopt;
    }

    // Take the host's resolved flags and rebind to the header file.
    auto cmd = std::move(host_results.front());
    cmd.source_file = workspace.path_pool.resolve(workspace.path_pool.intern(path)).data();

    // Inject preamble: for cc1 args insert after "-cc1", otherwise after driver.
    std::size_t inject_pos = 1;
    if(cmd.resolved.is_cc1 && cmd.resolved.flags.size() >= 2 &&
       cmd.resolved.flags[1] == llvm::StringRef("-cc1")) {
        inject_pos = 2;
    }
    cmd.resolved.flags.insert(cmd.resolved.flags.begin() + inject_pos,
                              ctx_ptr->preamble_path.c_str());
    cmd.resolved.flags.insert(cmd.resolved.flags.begin() + inject_pos, "-include");

    LOG_INFO("resolve_compile_command: header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
    return cmd;
}

std::optional<HeaderFileContext> Compiler::resolve_header_context(std::uint32_t header_path_id,
                                                                  Session* session) {
    // Find source files that transitively include this header.
    auto hosts = workspace.dep_graph.find_host_sources(header_path_id);
    if(hosts.empty()) {
        LOG_DEBUG("resolve_header_context: no host sources for path_id={}", header_path_id);
        return std::nullopt;
    }

    // If there's an active context override, prefer that host.
    std::uint32_t host_path_id = 0;
    std::vector<std::uint32_t> chain;
    if(session && session->active_context.has_value()) {
        auto preferred = *session->active_context;
        auto preferred_path = workspace.path_pool.resolve(preferred);
        auto results = workspace.cdb.lookup(preferred_path, {.suppress_logging = true});
        if(!results.empty()) {
            auto c = workspace.dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                chain = std::move(c);
            }
        }
    }

    // Fall back to the first available host that has a CDB entry.
    if(chain.empty()) {
        for(auto candidate: hosts) {
            auto candidate_path = workspace.path_pool.resolve(candidate);
            auto results = workspace.cdb.lookup(candidate_path, {.suppress_logging = true});
            if(results.empty())
                continue;
            auto c = workspace.dep_graph.find_include_chain(candidate, header_path_id);
            if(c.empty())
                continue;
            host_path_id = candidate;
            chain = std::move(c);
            break;
        }
    }

    if(chain.empty()) {
        LOG_DEBUG("resolve_header_context: no usable host with include chain for path_id={}",
                  header_path_id);
        return std::nullopt;
    }

    // Build preamble text: for each file in the chain except the last (target),
    // append all content up to (but not including) the line that includes the
    // next file in the chain.
    std::string preamble;
    for(std::size_t i = 0; i + 1 < chain.size(); ++i) {
        auto cur_id = chain[i];
        auto next_id = chain[i + 1];

        auto cur_path = workspace.path_pool.resolve(cur_id);
        auto next_path = workspace.path_pool.resolve(next_id);
        auto next_filename = llvm::sys::path::filename(next_path);

        // Prefer in-memory document text over disk content.
        // Use the session if this file matches the session's path, otherwise
        // fall back to disk.
        std::string content;
        // Note: we don't have the sessions map here, so we always read from disk
        // for intermediate chain files.  The session parameter only covers the
        // header file itself (the target), not intermediate files in the chain.
        auto buf = llvm::MemoryBuffer::getFile(cur_path);
        if(!buf) {
            LOG_WARN("resolve_header_context: cannot read {}", cur_path);
            return std::nullopt;
        }
        content = (*buf)->getBuffer().str();

        // Scan line by line for the #include that brings in next_filename.
        llvm::StringRef content_ref(content);
        std::size_t line_start = 0;
        std::size_t include_line_start = std::string::npos;
        while(line_start <= content_ref.size()) {
            auto newline_pos = content_ref.find('\n', line_start);
            auto line_end =
                (newline_pos == llvm::StringRef::npos) ? content_ref.size() : newline_pos;
            auto line = content_ref.slice(line_start, line_end).trim();

            if(line.starts_with("#include") || line.starts_with("# include")) {
                // Extract the filename from the #include directive.
                // Handles: #include "foo.h", #include <foo.h>, # include "foo.h"
                auto quote_start = line.find_first_of("\"<");
                auto quote_end = llvm::StringRef::npos;
                if(quote_start != llvm::StringRef::npos) {
                    char close = (line[quote_start] == '"') ? '"' : '>';
                    quote_end = line.find(close, quote_start + 1);
                }
                if(quote_start != llvm::StringRef::npos && quote_end != llvm::StringRef::npos) {
                    auto included = line.slice(quote_start + 1, quote_end);
                    auto included_filename = llvm::sys::path::filename(included);
                    if(included_filename == next_filename) {
                        include_line_start = line_start;
                        break;
                    }
                }
            }

            line_start =
                (newline_pos == llvm::StringRef::npos) ? content_ref.size() + 1 : newline_pos + 1;
        }

        // Emit a #line marker then all content before the include line.
        preamble += std::format("#line 1 \"{}\"\n", cur_path.str());
        if(include_line_start != std::string::npos) {
            preamble += content_ref.substr(0, include_line_start).str();
        } else {
            // No matching include line found — emit the whole file to be safe.
            LOG_DEBUG("resolve_header_context: include line for {} not found in {}, emitting full",
                      next_filename,
                      cur_path);
            preamble += content;
        }
    }

    // Hash the preamble and write to cache directory.
    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(preamble));
    auto preamble_filename = std::format("{:016x}.h", preamble_hash);
    auto preamble_dir = path::join(workspace.config.cache_dir, "header_context");
    auto preamble_path = path::join(preamble_dir, preamble_filename);

    if(!llvm::sys::fs::exists(preamble_path)) {
        auto ec = llvm::sys::fs::create_directories(preamble_dir);
        if(ec) {
            LOG_WARN("resolve_header_context: cannot create dir {}: {}",
                     preamble_dir,
                     ec.message());
            return std::nullopt;
        }
        if(auto result = fs::write(preamble_path, preamble); !result) {
            LOG_WARN("resolve_header_context: cannot write preamble {}: {}",
                     preamble_path,
                     result.error().message());
            return std::nullopt;
        }
        LOG_INFO("resolve_header_context: wrote preamble {} for header path_id={}",
                 preamble_path,
                 header_path_id);
    }

    return HeaderFileContext{host_path_id, preamble_path, preamble_hash};
}

std::string uri_to_path(const std::string& uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto path = parsed->file_path();
        if(path.has_value()) {
            return std::move(*path);
        }
    }
    return uri;
}

void Compiler::publish_diagnostics(const std::string& uri,
                                   int version,
                                   const et::serde::RawValue& diagnostics_json) {
    std::vector<protocol::Diagnostic> diagnostics;
    if(!diagnostics_json.empty()) {
        auto status = et::serde::json::from_json(diagnostics_json.data, diagnostics);
        if(!status) {
            LOG_WARN("Failed to deserialize diagnostics JSON for {}", uri);
        }
    }
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.version = version;
    params.diagnostics = std::move(diagnostics);
    peer.send_notification(params);
}

void Compiler::clear_diagnostics(const std::string& uri) {
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.diagnostics = {};
    peer.send_notification(params);
}

et::task<bool> Compiler::ensure_pch(Session& session, const CompileCommand& cmd) {
    auto path_id = session.path_id;
    auto path = workspace.path_pool.resolve(path_id);
    auto& text = session.text;
    auto bound = compute_preamble_bound(text);
    if(bound == 0) {
        // No preamble directives — PCH would be empty. Clear any stale entry.
        workspace.pch_active.erase(path_id);
        session.pch_ref.reset();
        co_return true;
    }

    // Content-addressed key: preamble text + compile flags.
    // ResolvedFlags already excludes source file path and -main-file-name,
    // so we can use it directly — no manual stripping needed.
    auto preamble_text = llvm::StringRef(text).substr(0, bound);
    auto artifact_key = ArtifactCache::compute_key(preamble_text, cmd.resolved.flags);

    // Deterministic content-addressed PCH path.
    auto pch_path = path::join(workspace.config.cache_dir,
                               "cache",
                               "pch",
                               std::format("{:016x}.pch", artifact_key));

    // Reuse existing artifact if preamble+flags match and deps haven't changed.
    if(auto it = workspace.pch_active.find(path_id); it != workspace.pch_active.end()) {
        auto& st = it->second;
        if(st.artifact_key == artifact_key &&
           !workspace.artifact_cache.deps_changed(workspace.path_pool, artifact_key)) {
            st.bound = bound;
            session.pch_ref = Session::PCHRef{artifact_key, bound};
            co_return true;
        }
    }

    // Also check artifact cache directly (another file may have built this).
    if(auto* cached = workspace.artifact_cache.lookup(artifact_key)) {
        if(!cached->path.empty() && llvm::sys::fs::exists(cached->path) &&
           !deps_changed(workspace.path_pool, cached->deps)) {
            auto& st = workspace.pch_active[path_id];
            st.artifact_key = artifact_key;
            st.bound = bound;
            session.pch_ref = Session::PCHRef{artifact_key, bound};
            co_return true;
        }
    }

    // Preamble incomplete (user still typing) — defer rebuild, reuse old PCH if available.
    if(!is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        if(auto it = workspace.pch_active.find(path_id); it != workspace.pch_active.end()) {
            auto* entry = workspace.artifact_cache.lookup(it->second.artifact_key);
            co_return entry && !entry->path.empty();
        }
        co_return false;
    }

    // If another coroutine is already building PCH for this file, wait for it.
    if(auto it = workspace.pch_active.find(path_id);
       it != workspace.pch_active.end() && it->second.building) {
        co_await it->second.building->wait();
        if(auto it2 = workspace.pch_active.find(path_id); it2 != workspace.pch_active.end()) {
            session.pch_ref = Session::PCHRef{it2->second.artifact_key, it2->second.bound};
            auto* entry = workspace.artifact_cache.lookup(it2->second.artifact_key);
            co_return entry && !entry->path.empty();
        }
        co_return false;
    }

    // Register in-flight build so concurrent requests wait on us.
    auto completion = std::make_shared<et::event>();
    workspace.pch_active[path_id].building = completion;

    // Build a new PCH via stateless worker.
    worker::BuildParams bp;
    bp.kind = worker::BuildKind::BuildPCH;
    bp.file = std::string(path);
    bp.directory = cmd.resolved.directory.str();
    bp.arguments = cmd.to_string_argv();
    bp.text = text;
    bp.preamble_bound = bound;
    bp.output_path = pch_path;

    LOG_DEBUG("Building PCH for {}, bound={}, output={}", path, bound, pch_path);

    auto result = co_await pool.send_stateless(bp);

    if(!result.has_value() || !result.value().success) {
        LOG_WARN("PCH build failed for {}: {}",
                 path,
                 result.has_value() ? result.value().error : result.error().message);
        workspace.pch_active[path_id].building.reset();
        completion->set();
        co_return false;
    }

    // Insert into artifact cache.
    ArtifactEntry artifact;
    artifact.path = result.value().output_path;
    artifact.deps = capture_deps_snapshot(workspace.path_pool, result.value().deps);
    workspace.artifact_cache.insert(artifact_key, std::move(artifact));

    // Update active state.
    auto& st = workspace.pch_active[path_id];
    st.artifact_key = artifact_key;
    st.bound = bound;
    st.building.reset();

    session.pch_ref = Session::PCHRef{artifact_key, bound};

    LOG_INFO("PCH built for {}: {}", path, result.value().output_path);

    // Persist cache metadata after successful build.
    workspace.save_cache();

    completion->set();
    co_return true;
}

/// Compile module dependencies, build/reuse PCH, and fill PCM paths.
/// Shared preparation step used by both ensure_compiled() (stateful path)
/// and forward_stateless() (completion/signatureHelp path).
et::task<bool> Compiler::ensure_deps(Session& session,
                                     const CompileCommand& cmd,
                                     std::pair<std::string, uint32_t>& pch,
                                     std::unordered_map<std::string, std::string>& pcms) {
    auto path_id = session.path_id;

    // Compile C++20 module dependencies (PCMs).
    if(workspace.compile_graph && !co_await workspace.compile_graph->compile_deps(path_id)) {
        co_return false;
    }

    // Scan buffer text for module imports that might not be in compile_graph yet.
    // When a user adds `import std;` without saving, the compile_graph (disk-based)
    // doesn't know about the new dependency. Scan the in-memory text to find them.
    {
        auto scan_result = scan(session.text);
        for(auto& mod_name: scan_result.modules) {
            if(mod_name.empty())
                continue;
            bool found = false;
            for(auto& [pid, name]: workspace.path_to_module) {
                if(name == mod_name) {
                    // If PCM not already built, try to build it.
                    if(workspace.pcm_active.find(pid) == workspace.pcm_active.end()) {
                        if(workspace.compile_graph && workspace.compile_graph->has_unit(pid)) {
                            co_await workspace.compile_graph->compile_deps(pid);
                        }
                    }
                    found = true;
                    break;
                }
            }
            if(!found) {
                LOG_DEBUG("Buffer imports unknown module '{}', skipping", mod_name);
            }
        }
    }

    // Build or reuse PCH.
    auto pch_ok = co_await ensure_pch(session, cmd);
    if(pch_ok) {
        if(auto pch_it = workspace.pch_active.find(path_id); pch_it != workspace.pch_active.end()) {
            auto* entry = workspace.artifact_cache.lookup(pch_it->second.artifact_key);
            if(entry) {
                pch = {entry->path, pch_it->second.bound};
            }
        }
    }

    // Fill all available PCM paths, excluding the file's own PCM
    // to avoid "multiple module declarations".
    workspace.fill_pcm_deps(pcms, path_id);

    co_return true;
}

bool Compiler::is_stale(const Session& session) {
    if(session.ast_deps.has_value() && deps_changed(workspace.path_pool, *session.ast_deps))
        return true;

    // Check PCH staleness via the artifact cache.
    if(session.pch_ref.has_value()) {
        if(workspace.artifact_cache.deps_changed(workspace.path_pool,
                                                 session.pch_ref->artifact_key))
            return true;
    }

    return false;
}

void Compiler::record_deps(Session& session, llvm::ArrayRef<std::string> deps) {
    session.ast_deps = capture_deps_snapshot(workspace.path_pool, deps);
}

/// Pull-based compilation entry point for user-opened files.
///
/// Called lazily by forward_query() / forward_build() before every
/// feature request (hover, semantic tokens, etc.). Guarantees that when it
/// returns true the stateful worker assigned to `path_id` holds an up-to-date
/// AST and diagnostics have been published to the client.
///
/// Lifecycle overview (pull-based model):
///
///   didOpen / didChange          – only update Session, mark ast_dirty
///   didSave                      – mark dependents dirty, queue indexing
///   feature request arrives      – calls ensure_compiled() first
///     1. Fast-path exit if AST is already clean (!ast_dirty).
///     2. Compile any C++20 module dependencies (PCMs) via CompileGraph.
///     3. Build / reuse the precompiled header (PCH) via ensure_pch().
///     4. Send CompileParams to the stateful worker, which builds the AST.
///     5. On success: publish diagnostics, clear ast_dirty, schedule indexing.
///     6. On generation mismatch (user edited during compile): keep dirty,
///        the next feature request will trigger another compile cycle.
///
/// Only the opened file itself is remapped (its in-memory text is sent to the
/// worker); every other file is read from disk by the compiler.
///
/// Concurrency: multiple concurrent feature requests for the same file will
/// each call ensure_compiled(). The first one launches a detached compile
/// task via loop.schedule(); subsequent ones wait on the shared event.
/// The detached task cannot be cancelled by LSP $/cancelRequest, preventing
/// the race where cancellation wakes all waiters and they all start compiles.
et::task<bool> Compiler::ensure_compiled(Session& session) {
    auto path_id = session.path_id;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} ast_dirty={}",
              path_id,
              session.version,
              session.generation,
              session.ast_dirty);

    if(!session.ast_dirty) {
        if(!is_stale(session)) {
            co_return true;
        }
        session.ast_dirty = true;
    }

    // If another compile is already in flight, wait for it.
    // This co_await may be cancelled by LSP $/cancelRequest — that's fine,
    // it just means this particular feature request is abandoned.  The
    // detached compile task keeps running independently.
    while(session.compiling) {
        auto pending = session.compiling;
        co_await pending->done.wait();
        if(!session.ast_dirty)
            co_return true;
    }

    // No compile in flight and AST is dirty — launch a detached compile task.
    // The detached task is scheduled via loop.schedule() so it is NOT subject
    // to LSP $/cancelRequest cancellation.  This eliminates the race where
    // cancellation fires the RAII guard, waking all waiters simultaneously
    // and causing them all to start new compiles.
    auto pending_compile = std::make_shared<Session::PendingCompile>();
    session.compiling = pending_compile;

    LOG_INFO("ensure_compiled: launching detached compile path_id={} gen={}",
             path_id,
             session.generation);

    // Capture path_id by value so the detached lambda can re-lookup the session
    // from the sessions map after co_await (DenseMap may invalidate pointers).
    loop.schedule([](Compiler* self,
                     std::uint32_t pid,
                     std::shared_ptr<Session::PendingCompile> pc) -> et::task<> {
        // Re-lookup session from the sessions map (pointer may have been
        // invalidated by DenseMap growth during co_await).
        auto find_session = [&]() -> Session* {
            auto it = self->sessions.find(pid);
            return it != self->sessions.end() ? &it->second : nullptr;
        };

        auto* sess = find_session();
        if(!sess) {
            pc->done.set();
            co_return;
        }

        auto finish_compile = [&]() {
            auto* s = find_session();
            if(s && s->compiling == pc) {
                s->compiling.reset();
            }
            LOG_INFO("ensure_compiled: finish_compile (detached) path_id={}", pid);
            pc->done.set();
        };

        auto gen = sess->generation;
        LOG_INFO("ensure_compiled: starting compile (detached) path_id={} gen={}", pid, gen);

        auto file_path = std::string(self->workspace.path_pool.resolve(pid));
        auto uri = lsp::URI::from_file_path(file_path);
        std::string uri_str = uri.has_value() ? uri->str() : file_path;

        auto cmd_opt = self->resolve_compile_command(file_path, sess);
        if(!cmd_opt) {
            finish_compile();
            co_return;
        }
        auto& cmd = *cmd_opt;

        worker::CompileParams params;
        params.path = file_path;
        params.version = sess->version;
        params.text = sess->text;
        params.directory = cmd.resolved.directory.str();
        params.arguments = cmd.to_string_argv();

        if(!co_await self->ensure_deps(*sess, cmd, params.pch, params.pcms)) {
            LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
            finish_compile();
            co_return;
        }

        // Re-lookup after co_await (DenseMap may have grown).
        sess = find_session();
        if(!sess) {
            pc->done.set();
            co_return;
        }

        auto result = co_await self->pool.send_stateful(pid, params);

        // Re-lookup after co_await.
        sess = find_session();
        if(!sess) {
            pc->done.set();
            co_return;
        }

        if(sess->generation != gen) {
            LOG_INFO("ensure_compiled: generation mismatch ({} vs {}) for {}",
                     sess->generation,
                     gen,
                     uri_str);
            finish_compile();
            co_return;
        }

        if(!result.has_value()) {
            LOG_WARN("Compile failed for {}: {}", uri_str, result.error().message);
            self->clear_diagnostics(uri_str);
            finish_compile();
            co_return;
        }

        sess->ast_dirty = false;
        pc->succeeded = true;
        self->record_deps(*sess, result.value().deps);

        // Store open file index from the stateful worker's TUIndex.
        if(!result.value().tu_index_data.empty()) {
            auto tu_index = index::TUIndex::from(result.value().tu_index_data.data());
            OpenFileIndex ofi;
            ofi.file_index = std::move(tu_index.main_file_index);
            ofi.symbols = std::move(tu_index.symbols);
            ofi.content = sess->text;
            ofi.mapper.emplace(ofi.content, lsp::PositionEncoding::UTF16);
            sess->file_index = std::move(ofi);
        }

        auto version = sess->version;
        finish_compile();

        // Publish diagnostics AFTER marking compile as done, so that concurrent
        // forward_query() calls can proceed immediately.
        self->publish_diagnostics(uri_str, version, result.value().diagnostics);
        if(self->on_indexing_needed)
            self->on_indexing_needed();
    }(this, path_id, pending_compile));

    // Wait for the detached compile to finish.  If this wait is cancelled
    // by LSP $/cancelRequest, the detached task continues unaffected.
    co_await pending_compile->done.wait();

    co_return !session.ast_dirty;
}

Compiler::RawResult Compiler::forward_query(worker::QueryKind kind,
                                            Session& session,
                                            std::optional<protocol::Position> position,
                                            std::optional<protocol::Range> range) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    // Cache text before co_await — session reference may dangle if didClose
    // erases the entry from the sessions map during suspension.
    auto text = session.text;

    if(!co_await ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }

    auto sit = sessions.find(path_id);
    if(sit == sessions.end() || sit->second.ast_dirty) {
        co_return serde_raw{"null"};
    }

    worker::QueryParams wp;
    wp.kind = kind;
    wp.path = path;

    lsp::PositionMapper mapper(text, lsp::PositionEncoding::UTF16);

    if(position) {
        auto offset = mapper.to_offset(*position);
        if(!offset)
            co_return serde_raw{"null"};
        wp.offset = *offset;
    }

    if(range) {
        auto start = mapper.to_offset(range->start);
        auto end = mapper.to_offset(range->end);
        if(start && end) {
            wp.range = {*start, *end};
        }
    }

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value());
}

Compiler::RawResult Compiler::forward_build(worker::BuildKind kind,
                                            const protocol::Position& position,
                                            Session& session) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    auto cmd_opt = resolve_compile_command(path, &session);
    if(!cmd_opt) {
        co_return serde_raw{};
    }
    auto& cmd = *cmd_opt;

    worker::BuildParams wp;
    wp.kind = kind;
    wp.file = path;
    // Cache session fields before co_await — session reference may dangle
    // if didClose erases the entry from the sessions map during suspension.
    wp.version = session.version;
    wp.text = session.text;
    wp.directory = cmd.resolved.directory.str();
    wp.arguments = cmd.to_string_argv();

    if(!co_await ensure_deps(session, cmd, wp.pch, wp.pcms)) {
        co_return serde_raw{};
    }

    // After co_await, verify session still exists.
    if(sessions.find(path_id) == sessions.end()) {
        co_return serde_raw{};
    }

    lsp::PositionMapper mapper(wp.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value().result_json);
}

Compiler::RawResult Compiler::handle_completion(const protocol::Position& position,
                                                Session& session) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    lsp::PositionMapper mapper(session.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(offset) {
        auto pctx = detect_completion_context(session.text, *offset);
        if(pctx.kind == CompletionContext::IncludeQuoted ||
           pctx.kind == CompletionContext::IncludeAngled) {
            auto cmd_opt = resolve_compile_command(path);
            if(!cmd_opt)
                co_return serde_raw{"[]"};

            auto search_config =
                extract_search_config(cmd_opt->resolved.flags, cmd_opt->resolved.directory);
            DirListingCache dir_cache;
            auto resolved = resolve_search_config(search_config, dir_cache);
            bool angled = (pctx.kind == CompletionContext::IncludeAngled);
            auto candidates = complete_include_path(resolved, pctx.prefix, angled, dir_cache);

            std::vector<protocol::CompletionItem> items;
            items.reserve(candidates.size());
            for(auto& c: candidates) {
                protocol::CompletionItem item;
                item.label = c.is_directory ? c.name + "/" : c.name;
                item.kind = protocol::CompletionItemKind::File;
                items.push_back(std::move(item));
            }
            auto json = et::serde::json::to_json<et::ipc::lsp_config>(items);
            co_return serde_raw{json ? std::move(*json) : "[]"};
        }
        if(pctx.kind == CompletionContext::Import) {
            auto module_names = complete_module_import(workspace.path_to_module, pctx.prefix);

            std::vector<protocol::CompletionItem> items;
            items.reserve(module_names.size());
            for(auto& name: module_names) {
                protocol::CompletionItem item;
                item.label = name;
                item.kind = protocol::CompletionItemKind::Module;
                item.insert_text = name + ";";
                items.push_back(std::move(item));
            }
            auto json = et::serde::json::to_json<et::ipc::lsp_config>(items);
            co_return serde_raw{json ? std::move(*json) : "[]"};
        }
    }

    co_return co_await forward_build(worker::BuildKind::Completion, position, session);
}

}  // namespace clice
