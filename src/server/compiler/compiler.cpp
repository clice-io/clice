#include "server/compiler/compiler.h"

#include <format>
#include <ranges>
#include <string>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "index/tu_index.h"
#include "server/protocol/worker.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include "clang/Basic/Version.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
using serde_raw = kota::codec::RawValue;

/// Render hash-input fragments into an unambiguous byte stream (length-
/// prefixed, so embedded NULs cannot create colliding splits) and return
/// the 32-hex xxh3_128bits cache key.
static std::string cache_key(std::initializer_list<llvm::StringRef> parts) {
    std::string input;
    for(auto part: parts) {
        input += std::format("{}:", part.size());
        input += part;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(input));
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

/// RAII completion of an in-flight PCH build registration: wakes waiters
/// and clears the building marker on every exit path — crucially also when
/// the coroutine is cancelled and its frame unwinds at a suspension point,
/// which would otherwise leave waiters suspended on the event forever.
struct BuildingGuard {
    Workspace& workspace;
    std::uint32_t path_id;
    std::shared_ptr<kota::event> completion;

    ~BuildingGuard() {
        // Reset only our own registration: the entry may have been erased
        // (didClose) and re-registered by a newer build in the meantime.
        if(auto it = workspace.pch_cache.find(path_id);
           it != workspace.pch_cache.end() && it->second.building == completion) {
            it->second.building.reset();
        }
        completion->set();
    }
};

/// Detect whether the cursor is inside a preamble directive (include/import).

Compiler::Compiler(kota::event_loop& loop, Workspace& workspace, WorkerPool& pool) :
    loop(loop), workspace(workspace), pool(pool) {}

Compiler::~Compiler() {
    workspace.cancel_all();
}

kota::task<> Compiler::stop() {
    compile_tasks.cancel();
    co_await compile_tasks.join();

    // Requests have unwound and released their interest; now tear down the
    // module compile graph's own unit tasks.
    if(workspace.compile_graph) {
        co_await workspace.compile_graph->shutdown();
    }
}

void Compiler::init_compile_graph() {
    if(workspace.path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        return;
    }

    // Lazy dependency resolver: scans a module file on demand to discover imports.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = workspace.path_pool.resolve(path_id);
        std::vector<std::string> rule_append, rule_remove;
        workspace.config.match_rules(file_path, rule_append, rule_remove);
        auto results =
            workspace.cdb.lookup(file_path, {.remove = rule_remove, .append = rule_append});
        if(results.empty())
            return {};
        workspace.toolchain.resolve_or_warn(results[0]);

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
    auto dispatch = [this](std::uint32_t path_id) -> kota::task<bool> {
        auto mod_it = workspace.path_to_module.find(path_id);
        if(mod_it == workspace.path_to_module.end())
            co_return false;

        auto file_path = std::string(workspace.path_pool.resolve(path_id));

        worker::BuildParams bp;
        bp.kind = worker::BuildKind::BuildPCM;
        bp.file = file_path;
        if(!fill_compile_args(file_path, bp.directory, bp.arguments))
            co_return false;

        if(!workspace.store) {
            LOG_WARN("BuildPCM skipped for module {}: cache store is unavailable", mod_it->second);
            co_return false;
        }

        // Deterministic content-addressed PCM key over the source path and
        // the frontend-relevant subset of the compile flags.
        auto safe_module_name = mod_it->second;
        std::ranges::replace(safe_module_name, ':', '-');
        auto pcm_key = std::format("{}-{}",
                                   safe_module_name,
                                   cache_key({clang::getClangFullVersion(),
                                              bp.directory,
                                              file_path,
                                              canonicalize(bp.arguments, ArgsProfile::Frontend)}));

        // Check if cached PCM is still valid.
        if(auto pcm_it = workspace.pcm_cache.find(path_id); pcm_it != workspace.pcm_cache.end()) {
            if(pcm_it->second.key == pcm_key && workspace.store->lookup("pcm", pcm_key) &&
               !deps_changed(workspace.path_pool, pcm_it->second.deps)) {
                workspace.pcm_paths[path_id] = pcm_it->second.path;
                co_return true;
            }
        }

        bp.module_name = mod_it->second;
        auto pending = workspace.store->begin_store("pcm", pcm_key);
        bp.output_path = pending.tmp_path;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        workspace.fill_pcm_deps(bp.pcms);

        auto result = co_await pool.send_stateless(bp);
        if(!result.has_value() || !result.value().success) {
            workspace.store->abort(pending);
            LOG_WARN("BuildPCM failed for module {}: {}",
                     mod_it->second,
                     result.has_value() ? result.value().error : result.error().message);
            co_return false;
        }

        // Commit on the thread pool: it fsyncs the freshly written PCM.
        auto committed =
            co_await kota::queue([&] { return workspace.store->commit(std::move(pending)); });
        if(!committed.has_value() || !committed.value().has_value()) {
            LOG_WARN("Failed to commit PCM for module {}", mod_it->second);
            co_return false;
        }

        auto pcm_path = std::move(committed.value().value());
        workspace.pcm_paths[path_id] = pcm_path;
        workspace.pcm_cache[path_id] = {
            pcm_path,
            pcm_key,
            capture_deps_snapshot(workspace.path_pool, result.value().deps)};
        LOG_INFO("Built PCM for module {}: {}", mod_it->second, pcm_path);

        // Persist cache metadata after successful build.
        workspace.save_cache();

        // Signal that new index data is available for background merge.
        if(on_indexing_needed)
            on_indexing_needed();

        co_return true;
    };

    workspace.compile_graph =
        std::make_unique<CompileGraph>(loop, std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", workspace.path_to_module.size());
}

bool Compiler::fill_compile_args(llvm::StringRef path,
                                 std::string& directory,
                                 std::vector<std::string>& arguments,
                                 Session* session) {
    auto path_id = workspace.path_pool.intern(path);

    // 1. If the session has an active header context via switchContext,
    //    use the host source's CDB entry with file path replaced and preamble injected.
    if(session && session->active_context.has_value()) {
        return fill_header_context_args(path, path_id, directory, arguments, session);
    }

    // 2. Normal CDB lookup for the file itself.
    //    Apply rules from config (append/remove flags based on file patterns).
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(path, rule_append, rule_remove);
    auto results = workspace.cdb.lookup(path, {.remove = rule_remove, .append = rule_append});
    if(!results.empty()) {
        workspace.toolchain.resolve_or_warn(results.front());
        auto& cmd = results.front();
        directory = cmd.resolved.directory.str();
        arguments = cmd.to_string_argv();
        return true;
    }

    // 3. No CDB entry — try automatic header context resolution.
    return fill_header_context_args(path, path_id, directory, arguments, session);
}

bool Compiler::fill_header_context_args(llvm::StringRef path,
                                        std::uint32_t path_id,
                                        std::string& directory,
                                        std::vector<std::string>& arguments,
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
            return false;
        }
        if(session) {
            session->header_context = std::move(*resolved);
            ctx_ptr = &*session->header_context;
        } else {
            // Background indexing path — no session to store on.
            // Use a temporary (caller will use it immediately).
            // Store in a local and return.
            static thread_local std::optional<HeaderFileContext> tl_ctx;
            tl_ctx = std::move(*resolved);
            ctx_ptr = &*tl_ctx;
        }
    }

    auto host_path = workspace.path_pool.resolve(ctx_ptr->host_path_id);
    // Apply rules matching the HEADER path (what the user is editing) on top of
    // the host's command — rules are expected to apply uniformly to every file.
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(path, rule_append, rule_remove);
    auto host_results =
        workspace.cdb.lookup(host_path, {.remove = rule_remove, .append = rule_append});
    if(host_results.empty()) {
        LOG_WARN("fill_header_context_args: host {} has no CDB entry", host_path);
        return false;
    }

    workspace.toolchain.resolve_or_warn(host_results.front());

    auto& host_cmd = host_results.front();
    directory = host_cmd.resolved.directory.str();

    // Replace source_file and inject -include preamble into flags directly.
    CompileCommand header_cmd = host_cmd;
    header_cmd.source_file = workspace.path_pool.resolve(path_id).data();

    // Inject -include <preamble> into flags: after "-cc1" for cc1, after driver otherwise.
    std::size_t inject_pos = header_cmd.resolved.is_cc1 ? 2 : 1;
    header_cmd.resolved.flags.insert(header_cmd.resolved.flags.begin() + inject_pos,
                                     ctx_ptr->preamble_path.c_str());
    header_cmd.resolved.flags.insert(header_cmd.resolved.flags.begin() + inject_pos, "-include");

    arguments = header_cmd.to_string_argv();

    LOG_INFO("fill_compile_args: header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
    return true;
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
        auto results = workspace.cdb.lookup(preferred_path);
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
            auto results = workspace.cdb.lookup(candidate_path);
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
        std::string content;
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
    auto preamble_dir = path::join(workspace.config.project.cache_dir, "header_context");
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
                                   const kota::codec::RawValue& diagnostics_json) {
    if(!peer)
        return;
    std::vector<protocol::Diagnostic> diagnostics;
    if(!diagnostics_json.empty()) {
        auto status = kota::codec::json::from_json(diagnostics_json.data, diagnostics);
        if(!status) {
            LOG_WARN("Failed to deserialize diagnostics JSON for {}", uri);
        }
    }
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.version = version;
    params.diagnostics = std::move(diagnostics);
    peer->send_notification(params);
}

void Compiler::clear_diagnostics(const std::string& uri) {
    if(!peer)
        return;
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.diagnostics = {};
    peer->send_notification(params);
}

kota::task<bool> Compiler::ensure_pch(Session& session,
                                      const std::string& directory,
                                      const std::vector<std::string>& arguments) {
    auto path_id = session.path_id;
    auto path = workspace.path_pool.resolve(path_id);
    auto& text = session.text;
    auto bound = compute_preamble_bound(text);
    if(bound == 0) {
        // No preamble directives — PCH would be empty. Clear any stale entry.
        workspace.pch_cache.erase(path_id);
        session.pch_ref.reset();
        co_return true;
    }

    // Key the PCH by preamble text plus the frontend-relevant compile flags,
    // so files with the same preamble text but different flags (-D, -I, -std)
    // produce separate PCHs.  The source file path stays out of the key so
    // files with identical preambles share one PCH — but its DIRECTORY (and
    // the working directory) must stay in: quote includes and relative paths
    // resolve against them, so equal preamble text in different directories
    // can mean different content.  The clang version guards against reusing
    // blobs a newer bundled clang would reject.
    auto preamble_text = llvm::StringRef(text).substr(0, bound);
    auto pch_key = cache_key({clang::getClangFullVersion(),
                              directory,
                              path::parent_path(path),
                              preamble_text,
                              canonicalize(arguments, ArgsProfile::Frontend)});

    // Reuse existing PCH if the key and deps haven't changed.  The store
    // lookup refreshes the blob's LRU position and catches eviction.
    if(auto it = workspace.pch_cache.find(path_id); it != workspace.pch_cache.end()) {
        auto& st = it->second;
        if(st.key == pch_key && !st.path.empty() && workspace.store &&
           workspace.store->lookup("pch", pch_key) && !deps_changed(workspace.path_pool, st.deps)) {
            st.bound = bound;
            session.pch_ref = Session::PCHRef{path_id, pch_key, bound};
            co_return true;
        }
    }

    // Preamble incomplete (user still typing) — defer rebuild, reuse old PCH if available.
    if(!is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        co_return workspace.pch_cache.count(path_id) && !workspace.pch_cache[path_id].path.empty();
    }

    // If another coroutine is already building PCH for this file, wait for it.
    if(auto it = workspace.pch_cache.find(path_id);
       it != workspace.pch_cache.end() && it->second.building) {
        co_await it->second.building->wait();
        if(auto it2 = workspace.pch_cache.find(path_id); it2 != workspace.pch_cache.end()) {
            session.pch_ref = Session::PCHRef{path_id, it2->second.key, it2->second.bound};
        }
        co_return workspace.pch_cache.count(path_id) && !workspace.pch_cache[path_id].path.empty();
    }

    // Register in-flight build so concurrent requests wait on us.  The
    // guard wakes them on every exit, including cancellation mid-await.
    auto completion = std::make_shared<kota::event>();
    workspace.pch_cache[path_id].building = completion;
    BuildingGuard guard{workspace, path_id, completion};

    if(!workspace.store) {
        LOG_WARN("PCH build skipped: cache store is unavailable");
        co_return false;
    }

    // Build a new PCH via stateless worker: it writes the blob to the tmp
    // path allocated here; the store commits (fsync + rename) on success.
    auto pending = workspace.store->begin_store("pch", pch_key);

    worker::BuildParams bp;
    bp.priority = worker::Priority::High;
    bp.kind = worker::BuildKind::BuildPCH;
    bp.file = std::string(path);
    bp.directory = directory;
    bp.arguments = arguments;
    bp.text = text;
    bp.preamble_bound = bound;
    bp.output_path = pending.tmp_path;

    LOG_DEBUG("Building PCH for {}, bound={}, key={}", path, bound, pch_key);

    auto result = co_await pool.send_stateless(bp);

    if(!result.has_value() || !result.value().success) {
        workspace.store->abort(pending);
        LOG_WARN("PCH build failed for {}: {}",
                 path,
                 result.has_value() ? result.value().error : result.error().message);
        co_return false;
    }

    // Commit on the thread pool: it fsyncs the freshly written PCH.
    auto committed =
        co_await kota::queue([&] { return workspace.store->commit(std::move(pending)); });
    if(!committed.has_value() || !committed.value().has_value()) {
        LOG_WARN("Failed to commit PCH for {}", path);
        co_return false;
    }

    auto& st = workspace.pch_cache[path_id];
    st.path = committed.value().value();
    st.bound = bound;
    st.key = pch_key;
    st.deps = capture_deps_snapshot(workspace.path_pool, result.value().deps);
    st.document_links_json = std::move(result.value().pch_links_json);

    session.pch_ref = Session::PCHRef{path_id, pch_key, bound};

    LOG_INFO("PCH built for {}: {}", path, st.path);

    // Persist cache metadata after successful build.
    workspace.save_cache();

    co_return true;
}

/// Compile module dependencies, build/reuse PCH, and fill PCM paths.
/// Shared preparation step used by both ensure_compiled() (stateful path)
/// and forward_stateless() (completion/signatureHelp path).
kota::task<bool> Compiler::ensure_deps(Session& session,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments,
                                       std::pair<std::string, uint32_t>& pch,
                                       std::unordered_map<std::string, std::string>& pcms,
                                       std::optional<kota::cancellation_token> scope) {
    auto path_id = session.path_id;

    // Compile module dependencies within the request scope: cancelling the
    // scope unwinds the wait and releases this request's interest in the
    // dependency graph, without touching the shared compilations themselves.
    auto compile_deps = [&](std::uint32_t pid) -> kota::task<bool> {
        if(!scope) {
            co_return co_await workspace.compile_graph->compile_deps(pid);
        }
        auto result = co_await kota::with_token(workspace.compile_graph->compile_deps(pid), *scope);
        co_return result.has_value() && *result;
    };

    // Re-validate cached PCM blobs and compile module dependencies.  LRU
    // eviction can remove a blob while its compile unit is still marked
    // clean, so dirty those units instead of handing clang a dangling
    // path.  Building dependencies can itself evict another clean
    // module's PCM under budget pressure, which reopens the window the
    // scan just closed — hence the bounded retry until the set is stable.
    if(workspace.compile_graph) {
        for(int attempt = 0; attempt < 3; ++attempt) {
            llvm::SmallVector<std::uint32_t> evicted;
            for(auto& [pid, pcm_path]: workspace.pcm_paths) {
                if(!llvm::sys::fs::exists(pcm_path)) {
                    evicted.push_back(pid);
                }
            }
            if(attempt > 0 && evicted.empty()) {
                break;
            }

            for(auto pid: evicted) {
                for(auto id: workspace.compile_graph->update(pid)) {
                    workspace.pcm_paths.erase(id);
                    workspace.pcm_cache.erase(id);
                }
                workspace.pcm_paths.erase(pid);
                workspace.pcm_cache.erase(pid);
            }

            if(!co_await compile_deps(path_id)) {
                co_return false;
            }
        }
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
                    if(workspace.pcm_paths.find(pid) == workspace.pcm_paths.end()) {
                        if(workspace.compile_graph && workspace.compile_graph->has_unit(pid)) {
                            co_await compile_deps(pid);
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

    // The buffer-scan waits above tolerate failed PCM builds, but a cancelled
    // scope means this round was superseded — abandon it before the PCH step.
    if(scope && scope->cancelled()) {
        co_return false;
    }

    // Build or reuse PCH.
    auto pch_ok = co_await ensure_pch(session, directory, arguments);
    if(pch_ok) {
        if(auto pch_it = workspace.pch_cache.find(path_id); pch_it != workspace.pch_cache.end()) {
            pch = {pch_it->second.path, pch_it->second.bound};
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

    // Check PCH staleness via the session's pch_ref.
    if(session.pch_ref.has_value()) {
        auto pch_it = workspace.pch_cache.find(session.pch_ref->path_id);
        if(pch_it != workspace.pch_cache.end() &&
           deps_changed(workspace.path_pool, pch_it->second.deps))
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
kota::task<> Compiler::run_compile(std::shared_ptr<Session> session) {
    auto pc = session->compiling;
    auto pid = session->path_id;
    auto gen = session->generation;

    auto finish_compile = [&]() {
        if(session->compiling == pc) {
            session->compiling.reset();
        }
        LOG_INFO("ensure_compiled: finish path_id={}", pid);
        pc->done.set();
    };

    LOG_INFO("ensure_compiled: starting compile path_id={} gen={}", pid, gen);

    auto file_path = std::string(workspace.path_pool.resolve(pid));
    auto uri = lsp::URI::from_file_path(file_path);
    std::string uri_str = uri.has_value() ? uri->str() : file_path;

    worker::CompileParams params;
    params.path = file_path;
    params.version = session->version;
    params.text = session->text;
    if(!fill_compile_args(file_path, params.directory, params.arguments, session.get())) {
        finish_compile();
        co_return;
    }

    bool deps_ok = co_await ensure_deps(*session,
                                        params.directory,
                                        params.arguments,
                                        params.pch,
                                        params.pcms,
                                        pc->deps_scope.token());
    pc->deps_done = true;
    if(!deps_ok) {
        LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
        finish_compile();
        co_return;
    }

    if(session->generation != gen) {
        LOG_INFO("ensure_compiled: superseded before send ({} vs {}) for {}",
                 session->generation,
                 gen,
                 uri_str);
        finish_compile();
        co_return;
    }

    auto result = co_await pool.send_stateful(pid, params);

    if(session->generation != gen) {
        LOG_INFO("ensure_compiled: generation mismatch ({} vs {}) for {}",
                 session->generation,
                 gen,
                 uri_str);
        finish_compile();
        co_return;
    }

    if(!result.has_value()) {
        LOG_WARN("Compile failed for {}: {}", uri_str, result.error().message);
        clear_diagnostics(uri_str);
        finish_compile();
        co_return;
    }

    session->ast_dirty = false;
    pc->succeeded = true;
    record_deps(*session, result.value().deps);

    if(!result.value().tu_index_data.empty()) {
        auto tu_index = index::TUIndex::from(result.value().tu_index_data.data());
        session->file_index = std::move(tu_index.main_file_index);
        session->symbols = std::move(tu_index.symbols);
    }

    auto version = session->version;
    finish_compile();

    publish_diagnostics(uri_str, version, result.value().diagnostics);
    if(on_indexing_needed)
        on_indexing_needed();
}

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
/// each call ensure_compiled(). The first one spawns a compile task into the
/// Compiler's task_group; subsequent ones wait on the shared event.
/// The spawned task is not cancelled by LSP $/cancelRequest, preventing
/// the race where cancellation wakes all waiters and they all start compiles.
kota::task<bool> Compiler::ensure_compiled(std::shared_ptr<Session> session) {
    auto path_id = session->path_id;
    auto gen = session->generation;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} ast_dirty={}",
              path_id,
              session->version,
              gen,
              session->ast_dirty);

    if(!session->ast_dirty) {
        if(!is_stale(*session)) {
            co_return true;
        }
        session->ast_dirty = true;
    }

    // If an up-to-date compile is already in flight, wait for it.
    // This co_await may be cancelled by LSP $/cancelRequest — that's fine,
    // it just means this particular feature request is abandoned.  The
    // detached compile task keeps running independently.
    while(session->compiling) {
        auto pending = session->compiling;
        if(pending->generation != session->generation && !pending->deps_done) {
            // The in-flight compile is stale (user edited since it started)
            // and still holds interest in the module graph — supersede it.
            // A stale compile already past its dependency phase is left to
            // finish instead: superseding it gains nothing (the worker send
            // is not cancellable), and waiting coalesces rapid edits into a
            // single follow-up compile at the latest generation.
            break;
        }
        co_await pending->done.wait();
        if(!session->ast_dirty)
            co_return true;
    }

    // If we fell through (not superseding) and the generation changed while
    // we were waiting, the session was closed or replaced — don't compile.
    if(!session->compiling && session->generation != gen) {
        co_return false;
    }

    auto superseded = session->compiling;
    auto pending_compile = std::make_shared<Session::PendingCompile>();
    pending_compile->generation = session->generation;
    session->compiling = pending_compile;

    LOG_INFO("ensure_compiled: launching compile path_id={} gen={}", path_id, session->generation);

    // Spawn the replacement before cancelling the superseded compile: the new
    // round acquires its module-dependency interest synchronously, so shared
    // dependencies never see their interest drop to zero across the swap.
    compile_tasks.spawn(run_compile(session));

    if(superseded) {
        superseded->deps_scope.cancel();
    }

    // Wait for the detached compile to finish.  If this wait is cancelled
    // by LSP $/cancelRequest, the detached task continues unaffected.
    co_await pending_compile->done.wait();

    co_return !session->ast_dirty;
}

Compiler::RawResult Compiler::forward_query(worker::QueryKind kind,
                                            std::shared_ptr<Session> session,
                                            std::optional<protocol::Position> position,
                                            std::optional<protocol::Range> range) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;
    auto map = session->line_map();

    if(!co_await ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }

    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }

    worker::QueryParams wp;
    wp.kind = kind;
    wp.path = path;

    if(position) {
        auto offset = map.to_offset(*position);
        if(!offset)
            co_return serde_raw{"null"};
        wp.offset = *offset;
    }

    if(range) {
        auto start = map.to_offset(range->start);
        auto end = map.to_offset(range->end);
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
                                            std::shared_ptr<Session> session) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    worker::BuildParams wp;
    wp.priority = worker::Priority::High;
    wp.kind = kind;
    wp.file = path;
    wp.version = session->version;
    wp.text = session->text;
    if(!fill_compile_args(path, wp.directory, wp.arguments, session.get())) {
        co_return serde_raw{};
    }

    if(!co_await ensure_deps(*session, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        co_return serde_raw{};
    }

    if(session->generation != gen) {
        co_return serde_raw{};
    }

    lsp::LineMap map(wp.text);
    auto offset = map.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value().result_json);
}

Compiler::RawResult Compiler::forward_format(std::shared_ptr<Session> session,
                                             std::optional<protocol::Range> range) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    worker::BuildParams wp;
    wp.priority = worker::Priority::High;
    wp.kind = worker::BuildKind::Format;
    wp.file = path;
    wp.text = session->text;

    if(range) {
        lsp::LineMap map(wp.text);
        auto begin = map.to_offset(range->start);
        auto end = map.to_offset(range->end);
        if(!begin || !end)
            co_return serde_raw{"null"};
        wp.format_range = {*begin, *end};
    }

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value()) {
        co_return serde_raw{"null"};
    }
    co_return std::move(result.value().result_json);
}

Compiler::RawResult Compiler::handle_completion(const protocol::Position& position,
                                                std::shared_ptr<Session> session) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    auto map = session->line_map();
    auto offset = map.to_offset(position);
    if(offset) {
        auto pctx = detect_completion_context(session->text, *offset);
        if(pctx.kind == CompletionContext::IncludeQuoted ||
           pctx.kind == CompletionContext::IncludeAngled) {
            std::string directory;
            std::vector<std::string> arguments;
            if(!fill_compile_args(path, directory, arguments))
                co_return serde_raw{"[]"};

            std::vector<const char*> args_ptrs;
            args_ptrs.reserve(arguments.size());
            for(auto& arg: arguments)
                args_ptrs.push_back(arg.c_str());

            auto search_config = extract_search_config(args_ptrs, directory);
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
            auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(items);
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
            auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(items);
            co_return serde_raw{json ? std::move(*json) : "[]"};
        }
    }

    co_return co_await forward_build(worker::BuildKind::Completion, position, std::move(session));
}

}  // namespace clice
