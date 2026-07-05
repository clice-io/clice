#include "server/context/context_resolver.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "server/session/session_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/preamble_synthesis.h"

#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

bool indicates_missing_context(llvm::ArrayRef<protocol::Diagnostic> diagnostics) {
    constexpr static llvm::StringRef codes[] = {
        "err_unknown_typename",
        "err_undeclared_var_use",
        "err_undeclared_var_use_suggest",
        "err_pp_unterminated_conditional",
    };
    for(auto& diag: diagnostics) {
        if(diag.severity != protocol::DiagnosticSeverity::Error || !diag.code.has_value()) {
            continue;
        }
        auto* code = std::get_if<std::string>(&*diag.code);
        if(code && llvm::is_contained(codes, *code)) {
            return true;
        }
    }
    return false;
}

/// Per-file command selection decision log: which tiers were tried, which one
/// was hit, and a hash of the final command for correlating later failures.
static void log_command_decision(llvm::StringRef path,
                                 llvm::ArrayRef<llvm::StringRef> tried,
                                 CommandSource source,
                                 llvm::ArrayRef<std::string> arguments) {
    if(logging::options.level > logging::Level::info)
        return;
    std::string joined;
    for(auto& arg: arguments) {
        joined += arg;
        joined += '\0';
    }
    LOG_INFO("compile_args: file={} tried=[{}] source={} args_hash={:016x}",
             path,
             llvm::join(tried, ","),
             source,
             llvm::xxh3_64bits(llvm::StringRef(joined)));
}

/// Pick the host CDB entry matching the session's pinned command hash
/// (multi-configuration hosts), defaulting to the first entry.
///
/// Published hashes are computed against host-path rules, while `results`
/// may carry header-path rules — match by index through a host-rules
/// lookup of the same entry set instead of hashing `results` directly.
static CompileCommand& pick_host_command(Workspace& workspace,
                                         llvm::StringRef host_path,
                                         llvm::SmallVector<CompileCommand>& results,
                                         const Session* session) {
    if(session && session->active_context.has_value() &&
       !session->active_context->command_hash.empty()) {
        std::vector<std::string> host_append, host_remove;
        workspace.config.match_rules(host_path, host_append, host_remove);
        auto canonical =
            workspace.cdb.lookup(host_path, {.remove = host_remove, .append = host_append});
        for(std::size_t i = 0; i < canonical.size() && i < results.size(); ++i) {
            if(canonical_command_hash(canonical[i].to_string_argv(),
                                      canonical[i].resolved.directory) ==
               session->active_context->command_hash) {
                return results[i];
            }
        }
    }
    return results.front();
}

/// Human-readable summary of the distinguishing flags of a command.
static std::string flags_label(const CompileCommand& cmd) {
    auto argv = cmd.to_argv();
    std::string desc;
    for(std::size_t j = 0; j < argv.size(); ++j) {
        llvm::StringRef a(argv[j]);
        if(a.starts_with("-D") || a.starts_with("-O") || a.starts_with("-std=") ||
           a.starts_with("-g")) {
            if(!desc.empty())
                desc += ' ';
            desc += argv[j];
            if((a == "-D" || a == "-O") && j + 1 < argv.size()) {
                desc += argv[++j];
            }
        }
    }
    return desc;
}

bool ContextResolver::fill_header_context_args(llvm::StringRef path,
                                               std::uint32_t path_id,
                                               std::string& directory,
                                               std::vector<std::string>& arguments,
                                               Session* session) {
    // Opening one of our own synthesized files (prefix/suffix/snapshot):
    // it is a fragment of the host TU it was synthesized for, so compile
    // it with that host's command, treated as self-contained. It must not
    // derive context from other synthesized state; without a recorded
    // host (e.g. a stale artifact from a wiped cache), fall through to
    // the default command.
    if(workspace.is_synthesized_artifact(path)) {
        auto it = workspace.synthesized_hosts.find(path);
        if(it == workspace.synthesized_hosts.end()) {
            return false;
        }
        auto host_path = workspace.path_pool.resolve(it->second);
        if(!workspace.cdb.has_entry(host_path)) {
            return false;
        }
        std::vector<std::string> rule_append, rule_remove;
        workspace.config.match_rules(path, rule_append, rule_remove);
        auto host_results =
            workspace.cdb.lookup(host_path, {.remove = rule_remove, .append = rule_append});
        workspace.toolchain.resolve_or_warn(host_results.front());
        CompileCommand artifact_cmd = host_results.front();
        artifact_cmd.source_file = workspace.path_pool.resolve(path_id).data();
        directory = artifact_cmd.resolved.directory.str();
        arguments = artifact_cmd.to_string_argv();
        return true;
    }

    // Self-containment routing: an Unknown or SelfContained header borrows
    // the host command without a prefix; NeedsContext synthesizes one.
    // run_compile() flips Unknown to NeedsContext when the trial compile's
    // diagnostics indicate missing includer state. An explicitly chosen
    // occurrence — even #0 — only has meaning under includer-context
    // semantics, so it forces synthesis regardless of the verdict.
    bool synthesize = workspace.header_mode(path, path_id) == HeaderMode::NeedsContext ||
                      (session && session->active_context.has_value() &&
                       session->active_context->occurrence.has_value());

    // Use cached context if it is still valid; otherwise resolve. The cache
    // is dropped when an active context override points to a different host
    // or include occurrence, when the routing mode changed, or when any
    // chain file changed on disk (the synthesized preamble embeds their
    // content, so it must be rebuilt).
    if(session && session->header_context.has_value()) {
        bool override_mismatch =
            session->active_context.has_value() &&
            (session->header_context->host_path_id != session->active_context->host_path_id ||
             session->header_context->occurrence !=
                 session->active_context->occurrence.value_or(0) ||
             session->header_context->host_command_hash != session->active_context->command_hash);
        bool mode_mismatch = session->header_context->preamble_path.empty() == synthesize;
        if(override_mismatch || mode_mismatch ||
           deps_changed(workspace.path_pool, session->header_context->deps)) {
            session->header_context.reset();
        }
    }

    std::optional<HeaderContext> local_ctx;
    const HeaderContext* ctx_ptr = nullptr;
    if(session && session->header_context.has_value()) {
        ctx_ptr = &*session->header_context;
    } else {
        auto resolved = resolve_header_context(path_id, session, synthesize);
        if(!resolved) {
            LOG_WARN("No CDB entry and no header context for {}", path);
            return false;
        }
        if(session) {
            session->header_context = std::move(*resolved);
            ctx_ptr = &*session->header_context;
        } else {
            // Background indexing path — no session to cache on.
            local_ctx = std::move(*resolved);
            ctx_ptr = &*local_ctx;
        }
    }

    auto host_path = workspace.path_pool.resolve(ctx_ptr->host_path_id);
    if(!workspace.cdb.has_entry(host_path)) {
        LOG_WARN("fill_header_context_args: host {} has no CDB entry", host_path);
        return false;
    }

    // Apply rules matching the HEADER path (what the user is editing) on top of
    // the host's command — rules are expected to apply uniformly to every file.
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(path, rule_append, rule_remove);
    auto host_results =
        workspace.cdb.lookup(host_path, {.remove = rule_remove, .append = rule_append});

    auto& host_cmd = pick_host_command(workspace, host_path, host_results, session);
    workspace.toolchain.resolve_or_warn(host_cmd);
    directory = host_cmd.resolved.directory.str();

    // Replace source_file; inject -include <preamble> only when a prefix
    // was synthesized (after "-cc1" for cc1, after the driver otherwise).
    CompileCommand header_cmd = host_cmd;
    header_cmd.source_file = workspace.path_pool.resolve(path_id).data();

    if(!ctx_ptr->preamble_path.empty()) {
        std::size_t inject_pos = header_cmd.resolved.is_cc1 ? 2 : 1;
        header_cmd.resolved.flags.insert(header_cmd.resolved.flags.begin() + inject_pos,
                                         ctx_ptr->preamble_path.c_str());
        header_cmd.resolved.flags.insert(header_cmd.resolved.flags.begin() + inject_pos,
                                         "-include");
    }

    arguments = header_cmd.to_string_argv();

    LOG_INFO("resolve_command: header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
    return true;
}

CommandSource ContextResolver::resolve_command(llvm::StringRef path,
                                               std::string& directory,
                                               std::vector<std::string>& arguments,
                                               Session* session) {
    auto path_id = workspace.path_pool.intern(path);
    llvm::SmallVector<llvm::StringRef, 3> tried;

    // Fill from the CDB layer with config rules applied (append/remove flags
    // based on file patterns). Also used for tier 4: lookup() synthesizes a
    // default command for files without an entry.
    auto fill_from_cdb = [&] {
        std::vector<std::string> rule_append, rule_remove;
        workspace.config.match_rules(path, rule_append, rule_remove);
        auto results = workspace.cdb.lookup(path, {.remove = rule_remove, .append = rule_append});
        auto* cmd = &results.front();
        // Multi-config projects: honor the user's chosen CDB entry, matched
        // by canonical command hash so the choice survives CDB reordering.
        if(session && session->active_command.has_value()) {
            for(auto& candidate: results) {
                if(canonical_command_hash(candidate.to_string_argv(),
                                          candidate.resolved.directory) ==
                   *session->active_command) {
                    cmd = &candidate;
                    break;
                }
            }
        }
        workspace.toolchain.resolve_or_warn(*cmd);
        directory = cmd->resolved.directory.str();
        arguments = cmd->to_string_argv();
    };

    // 1. If the session has an active header context via switchContext,
    //    use the host source's CDB entry with file path replaced and preamble injected.
    if(session && session->active_context.has_value()) {
        tried.push_back("switch_context");
        if(fill_header_context_args(path, path_id, directory, arguments, session)) {
            log_command_decision(path, tried, CommandSource::IncludeGraph, arguments);
            return CommandSource::IncludeGraph;
        }
    }

    // 2. Real CDB entry for the file itself (lookup() synthesizes a command
    //    for unknown files, so a non-empty result alone proves nothing).
    tried.push_back("cdb");
    if(workspace.cdb.has_entry(path)) {
        fill_from_cdb();
        log_command_decision(path, tried, CommandSource::CDBExact, arguments);
        return CommandSource::CDBExact;
    }

    // 3. No CDB entry — try automatic header context resolution.
    if(!(session && session->active_context.has_value())) {
        tried.push_back("include_graph");
        if(fill_header_context_args(path, path_id, directory, arguments, session)) {
            log_command_decision(path, tried, CommandSource::IncludeGraph, arguments);
            return CommandSource::IncludeGraph;
        }
    }

    // 4. Nothing matched — use the default command the CDB layer synthesizes
    //    for unknown files, so the file still compiles and produces
    //    diagnostics instead of failing silently.
    tried.push_back("fallback");
    fill_from_cdb();
    log_command_decision(path, tried, CommandSource::Fallback, arguments);
    return CommandSource::Fallback;
}

void ContextResolver::append_suffix_include(const Session& session, std::string& text) {
    if(!session.header_context.has_value() || session.header_context->suffix_path.empty()) {
        return;
    }
    if(!text.ends_with('\n')) {
        text += '\n';
    }
    text += "#include \"";
    // Escape like preamble_synthesis's line markers: Windows separators
    // must survive the preprocessor's string literal parsing.
    for(char c: session.header_context->suffix_path) {
        if(c == '\\' || c == '"') {
            text += '\\';
        }
        text += c;
    }
    text += "\"\n";
}

std::optional<HeaderContext> ContextResolver::resolve_header_context(std::uint32_t header_path_id,
                                                                     Session* session,
                                                                     bool synthesize) {
    // Find source files that transitively include this header.
    auto hosts = workspace.dep_graph.find_host_sources(header_path_id);
    if(hosts.empty()) {
        LOG_DEBUG("resolve_header_context: no host sources for path_id={}", header_path_id);
        return std::nullopt;
    }

    // If there's an active context override, prefer that host (and its
    // chosen include occurrence).
    std::uint32_t host_path_id = 0;
    std::optional<std::uint32_t> occurrence;
    std::vector<std::uint32_t> chain;
    if(session && session->active_context.has_value()) {
        auto preferred = session->active_context->host_path_id;
        auto preferred_path = workspace.path_pool.resolve(preferred);
        if(workspace.cdb.has_entry(preferred_path)) {
            auto c = workspace.dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                occurrence = session->active_context->occurrence;
                chain = std::move(c);
            }
        }
    }

    // Fall back to the most relevant host that has a real CDB entry —
    // a host with a synthesized command would just be a fallback in disguise.
    if(chain.empty()) {
        for(auto candidate: workspace.rank_hosts(header_path_id, hosts)) {
            auto candidate_path = workspace.path_pool.resolve(candidate);
            if(!workspace.cdb.has_entry(candidate_path))
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

    // Self-contained route: borrow the host's command, no prefix needed.
    // The chain is kept so a didSave along it still invalidates the session.
    std::string host_command_hash;
    if(session && session->active_context.has_value()) {
        host_command_hash = session->active_context->command_hash;
    }

    if(!synthesize) {
        llvm::SmallVector<std::uint32_t> chain_ids(chain.begin(), chain.end() - 1);
        return HeaderContext{host_path_id,
                             "",
                             0,
                             "",
                             occurrence.value_or(0),
                             std::move(host_command_hash),
                             std::move(chain_ids),
                             {}};
    }

    // Include directives along the chain are resolved with the host's real
    // search configuration, so same-named headers in different directories
    // cannot be confused.
    auto host_path = workspace.path_pool.resolve(host_path_id);
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(host_path, rule_append, rule_remove);
    auto host_results =
        workspace.cdb.lookup(host_path, {.remove = rule_remove, .append = rule_append});
    if(host_results.empty()) {
        return std::nullopt;
    }
    auto& resolve_cmd = pick_host_command(workspace, host_path, host_results, session);
    workspace.toolchain.resolve_or_warn(resolve_cmd);

    auto argv = resolve_cmd.to_argv();
    auto search_config = extract_search_config(argv, resolve_cmd.resolved.directory);
    DirListingCache dir_cache;
    auto resolved_config = resolve_search_config(search_config, dir_cache);

    auto resolver = [&](llvm::StringRef filename,
                        bool is_angled,
                        bool is_include_next,
                        llvm::StringRef includer_dir) -> std::optional<std::string> {
        auto entries = resolve_dir(includer_dir, dir_cache);
        auto result = resolve_include(filename,
                                      is_angled,
                                      entries,
                                      includer_dir,
                                      is_include_next,
                                      0,
                                      resolved_config,
                                      dir_cache);
        if(!result) {
            return std::nullopt;
        }
        // Normalize through the path pool: resolve_include builds native
        // separators, but chain paths compared against it are pool-normalized.
        return std::string(workspace.path_pool.resolve(workspace.path_pool.intern(result->path)));
    };

    // Read the chain files (all but the target) from disk. The synthesized
    // preamble deliberately reflects disk state, never open-document buffers:
    // open files must not be depended upon by other files. The staleness
    // snapshot timestamp is taken before the reads so a concurrent write
    // lands past build_at and triggers re-validation.
    auto build_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::vector<std::string> chain_contents;
    llvm::SmallVector<ChainEntry> chain_entries;
    chain_contents.reserve(chain.size() - 1);
    chain_entries.reserve(chain.size() - 1);
    for(std::size_t i = 0; i + 1 < chain.size(); ++i) {
        auto cur_path = workspace.path_pool.resolve(chain[i]);
        auto buf = llvm::MemoryBuffer::getFile(cur_path);
        if(!buf) {
            LOG_WARN("resolve_header_context: cannot read {}", cur_path);
            return std::nullopt;
        }
        chain_contents.emplace_back((*buf)->getBuffer());
        chain_entries.push_back({cur_path, chain_contents.back()});
    }

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the prefix/suffix must point at a copy.
    auto target_path = workspace.path_pool.resolve(chain.back());
    std::string self_snapshot_path;
    std::uint64_t target_hash = 0;
    auto preamble_dir = path::join(workspace.config.project.cache_dir, "header_context");
    if(auto target_buf = llvm::MemoryBuffer::getFile(target_path)) {
        auto content = (*target_buf)->getBuffer();
        target_hash = llvm::xxh3_64bits(content);
        self_snapshot_path = path::join(preamble_dir, std::format("{:016x}.self.h", target_hash));
        if(!llvm::sys::fs::exists(self_snapshot_path)) {
            auto ec = llvm::sys::fs::create_directories(preamble_dir);
            if(ec) {
                LOG_WARN("resolve_header_context: cannot create dir {}: {}",
                         preamble_dir,
                         ec.message());
                return std::nullopt;
            }
            if(auto result = fs::write(self_snapshot_path, content); !result) {
                LOG_WARN("resolve_header_context: cannot write snapshot {}: {}",
                         self_snapshot_path,
                         result.error().message());
                return std::nullopt;
            }
        }
    }

    if(!self_snapshot_path.empty()) {
        workspace.synthesized_hosts[self_snapshot_path] = host_path_id;
    }

    auto synthesized =
        synthesize_context(chain_entries, target_path, resolver, occurrence, self_snapshot_path);
    if(!synthesized) {
        LOG_WARN("resolve_header_context: cannot match include chain for {} (host={})",
                 target_path,
                 host_path);
        return std::nullopt;
    }
    auto& preamble = synthesized->prefix;

    // Hash the preamble and write to cache directory.
    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(preamble));
    auto preamble_filename = std::format("{:016x}.h", preamble_hash);
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
    workspace.synthesized_hosts[preamble_path] = host_path_id;

    // The suffix restores everything after the include position (closing
    // braces of enums/functions the fragment is embedded in). Injected by
    // appending one #include line to the header's buffer at compile time.
    std::string suffix_path;
    if(!synthesized->suffix.empty()) {
        auto suffix_hash = llvm::xxh3_64bits(llvm::StringRef(synthesized->suffix));
        suffix_path = path::join(preamble_dir, std::format("{:016x}.suffix.h", suffix_hash));
        if(!llvm::sys::fs::exists(suffix_path)) {
            if(auto result = fs::write(suffix_path, synthesized->suffix); !result) {
                LOG_WARN("resolve_header_context: cannot write suffix {}: {}",
                         suffix_path,
                         result.error().message());
                return std::nullopt;
            }
        }
        workspace.synthesized_hosts[suffix_path] = host_path_id;
    }

    // Snapshot the chain files for staleness detection: their content lives
    // inside the synthesized preamble, so clang's own dependency tracking
    // never sees them. Hash the buffers already read above — re-reading from
    // disk could capture content newer than what the preamble embeds.
    DepsSnapshot deps;
    deps.build_at = build_at;
    llvm::SmallVector<std::uint32_t> chain_ids(chain.begin(), chain.end() - 1);
    deps.path_ids.assign(chain_ids.begin(), chain_ids.end());
    deps.hashes.reserve(chain_contents.size() + 1);
    for(auto& content: chain_contents) {
        deps.hashes.push_back(llvm::xxh3_64bits(content));
    }
    if(!self_snapshot_path.empty()) {
        // The self-snapshot mirrors the header's disk state; re-synthesize
        // when it changes so other-occurrence expansions stay current.
        deps.path_ids.push_back(chain.back());
        deps.hashes.push_back(target_hash);
    }

    return HeaderContext{host_path_id,
                         preamble_path,
                         preamble_hash,
                         std::move(suffix_path),
                         occurrence.value_or(0),
                         std::move(host_command_hash),
                         std::move(chain_ids),
                         std::move(deps)};
}

void ContextResolver::restore_saved_context(Session& session) {
    auto path_id = session.path_id;
    auto path = workspace.path_pool.resolve(path_id);

    // Restore a context choice persisted from an earlier session. The
    // CDB or include graph may have changed while the server was down —
    // validate before adopting, since a stale active_context suppresses
    // automatic host resolution and strands the file on the fallback
    // command.
    if(auto it = workspace.saved_contexts.find(path_id); it != workspace.saved_contexts.end()) {
        auto& ws = workspace;
        auto& saved = it->second;
        auto entry_has_hash = [&ws](llvm::StringRef entry_path, llvm::StringRef hash) {
            std::vector<std::string> rule_append, rule_remove;
            ws.config.match_rules(entry_path, rule_append, rule_remove);
            for(auto& cmd:
                ws.cdb.lookup(entry_path, {.remove = rule_remove, .append = rule_append})) {
                if(canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory) == hash) {
                    return true;
                }
            }
            return false;
        };

        bool valid = false;
        if(saved.host_path_id != no_path_id) {
            auto host_path = ws.path_pool.resolve(saved.host_path_id);
            valid = ws.cdb.has_entry(host_path) &&
                    !ws.dep_graph.find_include_chain(saved.host_path_id, path_id).empty() &&
                    (saved.command_hash.empty() || entry_has_hash(host_path, saved.command_hash));
            if(valid) {
                session.active_context = Session::ActiveContext{saved.host_path_id,
                                                                saved.occurrence,
                                                                saved.command_hash};
            }
        } else if(!saved.command_hash.empty()) {
            valid = ws.cdb.has_entry(path) && entry_has_hash(path, saved.command_hash);
            if(valid) {
                session.active_command = saved.command_hash;
            }
        }
        if(!valid) {
            LOG_INFO("didOpen: dropping stale saved context for {}", path);
            workspace.saved_contexts.erase(it);
        }
    }
}

bool ContextResolver::drop_orphaned_choices(SessionStore& sessions) {
    bool dropped_saved = false;
    for(auto& [session_id, session]: sessions.sessions) {
        if(!session->active_context.has_value()) {
            continue;
        }
        auto host_id = session->active_context->host_path_id;
        auto& occurrence = session->active_context->occurrence;
        bool orphaned = workspace.dep_graph.find_include_chain(host_id, session_id).empty();
        // A pinned occurrence can vanish while other inclusions of the
        // header survive (the chain stays non-empty) — recount it.
        if(!orphaned && occurrence.has_value()) {
            auto count = workspace.count_occurrences(host_id, session_id);
            orphaned = count > 0 && *occurrence >= count;
        }
        if(orphaned) {
            LOG_INFO("Dropping orphaned context choice for {}: host {} no longer includes it",
                     workspace.path_pool.resolve(session_id),
                     workspace.path_pool.resolve(host_id));
            session->active_context.reset();
            session->header_context.reset();
            session->pch_ref.reset();
            session->ast_dirty = true;
            session->trial_done = false;
            // Invalidate in-flight compiles so they cannot clobber the
            // reset state when they finish (same as switchContext).
            session->generation += 1;
            dropped_saved |= workspace.saved_contexts.erase(session_id) > 0;
        }
    }
    return dropped_saved;
}

ext::QueryContextResult ContextResolver::query_contexts(llvm::StringRef path,
                                                        std::uint32_t path_id,
                                                        const ext::QueryContextParams& params) {
    auto& ws = workspace;
    int offset_val = std::max(0, params.offset.value_or(0));
    constexpr int page_size = 10;

    ext::QueryContextResult result;
    std::vector<ext::ContextItem> all_items;

    // Contexts that would produce identical compilation results are
    // collapsed: identical canonical flags mean an identical compile
    // — but only for headers CONFIRMED self-contained. A header that
    // needs includer context gets a different synthesized prefix per
    // host, and an un-trialed header may turn out the same way, so
    // every host stays a distinct context for both.
    llvm::StringSet<> seen_configs;
    bool dedup_hosts = ws.header_mode(path, path_id) == HeaderMode::SelfContained;

    auto hosts = ws.dep_graph.find_host_sources(path_id);
    for(auto host_id: ws.rank_hosts(path_id, hosts)) {
        auto host_path = ws.path_pool.resolve(host_id);
        if(!ws.cdb.has_entry(host_path))
            continue;
        auto host_uri_opt = lsp::URI::from_file_path(std::string(host_path));
        if(!host_uri_opt)
            continue;

        // A multi-configuration host contributes one context per
        // CDB entry: each configuration compiles the header under
        // different preprocessor state.
        std::vector<std::string> host_append, host_remove;
        ws.config.match_rules(host_path, host_append, host_remove);
        auto cmds = ws.cdb.lookup(host_path, {.remove = host_remove, .append = host_append});
        auto occurrences = ws.count_occurrences(host_id, path_id);

        for(auto& cmd: cmds) {
            auto hash = canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory);
            if(dedup_hosts && !seen_configs.insert(hash).second)
                continue;

            ext::ContextItem item;
            item.label = llvm::sys::path::filename(host_path).str();
            if(cmds.size() > 1) {
                auto desc = flags_label(cmd);
                if(!desc.empty()) {
                    item.label = std::format("{} [{}]", item.label, desc);
                }
                item.command_hash = hash;
            }
            item.description = std::string(host_path);
            item.uri = host_uri_opt->str();

            // A guard-less header can be included several times by
            // one host — each occurrence is a distinct context.
            if(occurrences > 1) {
                for(std::uint32_t n = 0; n < occurrences; ++n) {
                    auto occ_item = item;
                    occ_item.label = std::format("{} (#{})", item.label, n + 1);
                    occ_item.occurrence = n;
                    all_items.push_back(std::move(occ_item));
                }
            } else {
                all_items.push_back(std::move(item));
            }
        }
    }

    // Real entries only: lookup() would synthesize a default command
    // even for unknown files, offering a bogus context that
    // switchContext would then reject. Offered even when hosts
    // exist, so a host override can be switched back to the file's
    // own command.
    if(ws.cdb.has_entry(path)) {
        std::vector<std::string> rule_append, rule_remove;
        ws.config.match_rules(path, rule_append, rule_remove);
        auto entries = ws.cdb.lookup(path, {.remove = rule_remove, .append = rule_append});
        auto uri_opt = lsp::URI::from_file_path(std::string(path));
        for(std::size_t i = 0; uri_opt && i < entries.size(); ++i) {
            auto& cmd = entries[i];
            auto hash = canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory);
            if(!seen_configs.insert(hash).second)
                continue;

            auto desc = flags_label(cmd);
            ext::ContextItem item;
            item.label = desc.empty() ? std::format("config #{}", i) : desc;
            item.description = cmd.resolved.directory.str();
            item.uri = uri_opt->str();
            item.command_hash = std::move(hash);
            all_items.push_back(std::move(item));
        }
    }

    result.epoch = ws.context_epoch;
    result.total = static_cast<int>(all_items.size());
    int end = std::min(offset_val + page_size, static_cast<int>(all_items.size()));
    for(int i = offset_val; i < end; ++i) {
        result.contexts.push_back(std::move(all_items[i]));
    }
    return result;
}

ext::CurrentContextResult
    ContextResolver::current_context(llvm::StringRef path,
                                     const Session* session,
                                     const ext::CurrentContextParams& params) {
    ext::CurrentContextResult result;
    if(session && session->active_context) {
        auto& active = *session->active_context;
        auto ctx_path = workspace.path_pool.resolve(active.host_path_id);
        auto ctx_uri_opt = lsp::URI::from_file_path(std::string(ctx_path));
        if(ctx_uri_opt) {
            ext::ContextItem item;
            item.label = llvm::sys::path::filename(ctx_path).str();
            if(active.occurrence.value_or(0) > 0) {
                item.label = std::format("{} (#{})", item.label, *active.occurrence + 1);
            }
            item.description = std::string(ctx_path);
            item.uri = ctx_uri_opt->str();
            item.occurrence = active.occurrence;
            if(!active.command_hash.empty()) {
                item.command_hash = active.command_hash;
            }
            result.context = std::move(item);
        }
    } else if(session && session->active_command) {
        auto& ws = workspace;
        ext::ContextItem item;
        item.uri = params.uri;
        item.command_hash = *session->active_command;
        item.label = std::format("config {}", session->active_command->substr(0, 8));
        if(ws.cdb.has_entry(path)) {
            std::vector<std::string> rule_append, rule_remove;
            ws.config.match_rules(path, rule_append, rule_remove);
            for(auto& cmd: ws.cdb.lookup(path, {.remove = rule_remove, .append = rule_append})) {
                if(canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory) ==
                   *session->active_command) {
                    auto desc = flags_label(cmd);
                    if(!desc.empty()) {
                        item.label = std::move(desc);
                    }
                    item.description = cmd.resolved.directory.str();
                    break;
                }
            }
        }
        result.context = std::move(item);
    }
    return result;
}

ext::SwitchContextResult ContextResolver::switch_context(llvm::StringRef path,
                                                         std::uint32_t path_id,
                                                         Session* session,
                                                         llvm::StringRef context_path,
                                                         std::uint32_t context_path_id,
                                                         const ext::SwitchContextParams& params) {
    auto& ws = workspace;

    ext::SwitchContextResult result;

    // A choice made against an outdated listing may reference
    // contexts that no longer exist — make the client re-query.
    if(params.epoch.has_value() && *params.epoch != ws.context_epoch) {
        result.stale = true;
        return result;
    }

    if(!session) {
        return result;
    }

    // Validate that `hash` names a real CDB entry of `entry_path`.
    auto has_command = [&](llvm::StringRef entry_path, llvm::StringRef hash) {
        if(!ws.cdb.has_entry(entry_path)) {
            return false;
        }
        std::vector<std::string> rule_append, rule_remove;
        ws.config.match_rules(entry_path, rule_append, rule_remove);
        for(auto& cmd: ws.cdb.lookup(entry_path, {.remove = rule_remove, .append = rule_append})) {
            if(canonical_command_hash(cmd.to_string_argv(), cmd.resolved.directory) == hash) {
                return true;
            }
        }
        return false;
    };

    if(context_path_id == path_id && params.command_hash.has_value()) {
        // Pin one of the file's own CDB entries.
        if(!has_command(path, *params.command_hash)) {
            return result;
        }
        session->active_command = *params.command_hash;
        session->active_context.reset();
    } else {
        // Pin a host source for a header: it must have a real CDB
        // entry, actually (transitively) include this header, and —
        // for multi-configuration hosts — own the pinned entry.
        if(!ws.cdb.has_entry(context_path)) {
            return result;
        }
        if(ws.dep_graph.find_include_chain(context_path_id, path_id).empty()) {
            return result;
        }
        if(params.command_hash.has_value() && !has_command(context_path, *params.command_hash)) {
            return result;
        }
        if(params.occurrence.has_value() && *params.occurrence > 0) {
            auto count = ws.count_occurrences(context_path_id, path_id);
            if(count > 0 && *params.occurrence >= count) {
                return result;
            }
        }
        session->active_context = Session::ActiveContext{context_path_id,
                                                         params.occurrence,
                                                         params.command_hash.value_or("")};
        session->active_command.reset();
    }

    session->header_context.reset();
    session->pch_ref.reset();
    session->ast_deps.reset();
    session->ast_dirty = true;
    // The new context needs its own self-containment trial — a
    // different host can change the macro environment.
    session->trial_done = false;
    ws.forget_self_contained(path_id);
    // Invalidate any in-flight compile: without the bump it would
    // clobber ast_dirty on completion and publish results for the
    // old context, with nothing left for is_stale() to detect.
    session->generation++;

    // Persist the choice across sessions.
    SavedContext saved;
    if(session->active_context) {
        saved.host_path_id = session->active_context->host_path_id;
        saved.occurrence = session->active_context->occurrence;
        saved.command_hash = session->active_context->command_hash;
    } else if(session->active_command) {
        saved.command_hash = *session->active_command;
    }
    ws.saved_contexts[path_id] = std::move(saved);
    ws.save_cache();

    result.success = true;
    return result;
}

}  // namespace clice
