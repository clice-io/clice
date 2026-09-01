#include "sched/context.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/preamble_synthesis.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

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
/// (multi-configuration hosts), defaulting to the first candidate.
///
/// Published hashes are computed against host-path rules, while the caller
/// will apply header-path rules — so the pin is validated in the host-rules
/// context and the winning base config returned for the caller to re-derive.
static ConfigID pick_host_config(Workspace& workspace,
                                 llvm::StringRef host_path,
                                 llvm::ArrayRef<CompilationEntry> candidates,
                                 llvm::StringRef pinned_hash,
                                 llvm::StringRef pinned_base) {
    // The base identity resolved at pin time is exact; the applied hash
    // remains as the fallback for pins saved before the base was recorded
    // (and cannot distinguish candidates the rules collapse together).
    if(!pinned_base.empty()) {
        for(auto& entry: candidates) {
            if(workspace.cdb.entry_hash_hex(entry.config) == pinned_base) {
                return entry.config;
            }
        }
    }
    if(!pinned_hash.empty()) {
        std::vector<std::string> host_append, host_remove;
        workspace.config.match_rules(host_path, host_append, host_remove);
        for(auto& entry: candidates) {
            auto applied =
                workspace.cdb.apply_rules(entry.config,
                                          {.remove = host_remove, .append = host_append});
            if(workspace.cdb.entry_hash_hex(applied) == pinned_hash) {
                return entry.config;
            }
        }
    }
    return candidates.front().config;
}

HeaderMode ContextResolver::header_mode(llvm::StringRef path, Fid path_id) const {
    // Keep in sync with the client's C++ fragment detection
    // (editors/vscode/src/feature/context.ts).
    if(path.ends_with(".def") || path.ends_with(".inc") || path.ends_with(".inl") ||
       path.ends_with(".tpp") || path.ends_with(".ipp")) {
        return HeaderMode::NeedsContext;
    }
    if(auto it = header_modes.find(path_id); it != header_modes.end()) {
        return it->second;
    }
    return HeaderMode::Unknown;
}

void ContextResolver::forget_self_contained(Fid path_id) {
    if(auto it = header_modes.find(path_id);
       it != header_modes.end() && it->second == HeaderMode::SelfContained) {
        header_modes.erase(it);
    }
}

std::uint64_t ContextResolver::persisted_mode_hash(Fid path_id) const {
    if(header_modes.lookup(path_id) != HeaderMode::NeedsContext) {
        return 0;
    }
    return header_mode_hashes.lookup(path_id);
}

void ContextResolver::record_header_mode(Fid path_id, HeaderMode mode, std::uint64_t content_hash) {
    auto persisted = persisted_mode_hash(path_id);
    header_modes[path_id] = mode;
    if(mode == HeaderMode::NeedsContext) {
        header_mode_hashes[path_id] = content_hash;
    }
    if(persisted_mode_hash(path_id) != persisted) {
        workspace.mark_artifacts_dirty();
    }
}

void ContextResolver::reset_header_mode(Fid path_id) {
    if(persisted_mode_hash(path_id) != 0) {
        workspace.mark_artifacts_dirty();
    }
    header_modes.erase(path_id);
    header_mode_hashes.erase(path_id);
}

void ContextResolver::dump_mode_slices(std::vector<CacheModeEntry>& modes,
                                       llvm::function_ref<std::uint32_t(Fid)> intern_id) const {
    for(auto& [path_id, mode]: header_modes) {
        if(mode != HeaderMode::NeedsContext)
            continue;
        // A verdict scored with no disk observation (hash 0) cannot be
        // validated on load, so it stays in memory: persisted, it would
        // skip the self-containment trial for whatever bytes the next
        // session finds on disk.
        auto hash = header_mode_hashes.lookup(path_id);
        if(hash == 0)
            continue;
        modes.push_back({intern_id(path_id), static_cast<std::uint32_t>(mode), hash});
    }
}

void ContextResolver::dump_choice_slices(
    std::vector<CacheContextEntry>& contexts,
    std::vector<CacheArtifactEntry>& artifacts,
    llvm::function_ref<std::uint32_t(Fid)> intern_id,
    llvm::function_ref<std::uint32_t(llvm::StringRef)> intern_path) const {
    for(auto& entry: synthesized_hosts) {
        artifacts.push_back({intern_path(entry.getKey()), intern_id(entry.second)});
    }

    for(auto& [path_id, saved]: saved_contexts) {
        CacheContextEntry entry;
        entry.file = intern_id(path_id);
        entry.host = saved.host_path_id.valid() ? intern_id(saved.host_path_id) : ~0u;
        entry.occurrence = saved.occurrence.value_or(~0u);
        entry.command_hash = saved.command_hash;
        entry.base_hash = saved.base_hash;
        contexts.push_back(std::move(entry));
    }
}

void ContextResolver::load_mode_slices(llvm::ArrayRef<CacheModeEntry> modes,
                                       llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve) {
    for(auto& entry: modes) {
        auto file = resolve(entry.file);
        // The writer never emits unbound (hash 0) verdicts; an entry
        // carrying one is corrupt and must not bypass the content gate.
        if(file.empty() || entry.content_hash == 0 ||
           static_cast<HeaderMode>(entry.mode) != HeaderMode::NeedsContext)
            continue;
        auto id = workspace.file_table.intern(file);
        // The verdict is tied to the header's contents — a file edited
        // while the server was down must re-earn its trial.
        auto disk = workspace.file_table.current(id);
        if(!disk || disk->hash != entry.content_hash)
            continue;
        header_modes[id] = HeaderMode::NeedsContext;
        header_mode_hashes[id] = entry.content_hash;
    }
}

void ContextResolver::load_choice_slices(
    llvm::ArrayRef<CacheContextEntry> contexts,
    llvm::ArrayRef<CacheArtifactEntry> artifacts,
    llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve) {
    for(auto& entry: contexts) {
        auto file = resolve(entry.file);
        if(file.empty())
            continue;
        SavedContext saved;
        if(entry.host != ~0u) {
            auto host = resolve(entry.host);
            if(host.empty())
                continue;
            saved.host_path_id = workspace.file_table.intern(host);
        }
        if(entry.occurrence != ~0u) {
            saved.occurrence = entry.occurrence;
        }
        saved.command_hash = entry.command_hash;
        saved.base_hash = entry.base_hash;
        saved_contexts[workspace.file_table.intern(file)] = std::move(saved);
    }

    for(auto& entry: artifacts) {
        auto file = resolve(entry.file);
        auto host = resolve(entry.host);
        if(file.empty() || host.empty())
            continue;
        synthesized_hosts[file] = workspace.file_table.intern(host);
    }
}

void ContextResolver::record_synthesized_host(llvm::StringRef path, Fid host_path_id) {
    auto [it, inserted] = synthesized_hosts.try_emplace(path, host_path_id);
    if(!inserted && it->second == host_path_id) {
        return;
    }
    it->second = host_path_id;
    workspace.mark_contexts_dirty();
}

bool ContextResolver::fill_header_context_args(llvm::StringRef path,
                                               Fid path_id,
                                               std::string& directory,
                                               std::vector<std::string>& arguments,
                                               ContextUse use,
                                               Fid* host_path_id,
                                               CommandRef* out_ref) {
    // Opening one of our own synthesized files (prefix/suffix/snapshot):
    // it is a fragment of the host TU it was synthesized for, so compile
    // it with that host's command, treated as self-contained. It must not
    // derive context from other synthesized state; without a recorded
    // host (e.g. a stale artifact from a wiped cache), fall through to
    // the default command.
    if(workspace.is_synthesized_artifact(path)) {
        auto it = synthesized_hosts.find(path);
        if(it == synthesized_hosts.end()) {
            return false;
        }
        auto host_path = workspace.file_table.resolve(it->second);
        auto candidates = workspace.cdb.candidate_entries(host_path);
        if(candidates.empty()) {
            return false;
        }
        std::vector<std::string> rule_append, rule_remove;
        workspace.config.match_rules(path, rule_append, rule_remove);
        auto applied = workspace.cdb.apply_rules(candidates.front().config,
                                                 {.remove = rule_remove, .append = rule_append});
        // The artifact is a fragment of the host TU: it compiles as the
        // host's language, with the artifact path injected as the input.
        CommandRef ref{path_id,
                       applied,
                       workspace.cdb.input_kind(applied, host_path),
                       CommandSource::IncludeGraph};
        directory = workspace.cdb.config(applied).directory;
        arguments = to_strings(workspace.cdb.render(ref));
        if(host_path_id) {
            *host_path_id = it->second;
        }
        if(out_ref) {
            *out_ref = ref;
        }
        return true;
    }

    // Self-containment routing: an Unknown or SelfContained header borrows
    // the host command without a prefix; NeedsContext synthesizes one.
    // run_compile() flips Unknown to NeedsContext when the trial compile's
    // diagnostics indicate missing includer state. An explicitly chosen
    // occurrence — even #0 — only has meaning under includer-context
    // semantics, so it forces synthesis regardless of the verdict.
    const SavedContext* choice = active_choice(use, path_id);
    bool has_host_choice = choice && choice->host_path_id.valid();
    bool synthesize = header_mode(path, path_id) == HeaderMode::NeedsContext ||
                      (has_host_choice && choice->occurrence.has_value());

    // Use cached context if it is still valid; otherwise resolve. The cache
    // is dropped when an active context override points to a different host
    // or include occurrence, when the routing mode changed, or when any
    // chain file changed on disk (the synthesized preamble embeds their
    // content, so it must be rebuilt). Only editor-facing compiles consult
    // the cache; background indexing must stay independent of per-editor
    // context state, so it resolves fresh every time.
    if(use == ContextUse::Editor) {
        if(auto* cached = header_context(path_id)) {
            bool override_mismatch =
                has_host_choice && (cached->host_path_id != choice->host_path_id ||
                                    cached->occurrence != choice->occurrence.value_or(0) ||
                                    cached->host_command_hash != choice->command_hash ||
                                    cached->host_base_hash != choice->base_hash);
            bool mode_mismatch = cached->preamble_path.empty() == synthesize;
            auto wave = workspace.file_table.wave();
            if(override_mismatch || mode_mismatch ||
               deps_changed(workspace.file_table, cached->deps)) {
                drop_header_context(path_id);
            }
        }
    }

    std::optional<HeaderContext> local_ctx;
    const HeaderContext* ctx_ptr = use == ContextUse::Editor ? header_context(path_id) : nullptr;
    if(!ctx_ptr) {
        auto resolved = resolve_header_context(path_id, use, synthesize);
        if(!resolved) {
            LOG_WARN("No CDB entry and no header context for {}", path);
            return false;
        }
        if(use == ContextUse::Editor) {
            ctx_ptr = &(header_contexts[path_id] = std::move(*resolved));
        } else {
            // Background indexing stays independent of per-editor context
            // state: resolve fresh, cache nothing.
            local_ctx = std::move(*resolved);
            ctx_ptr = &*local_ctx;
        }
    }

    auto host_path = workspace.file_table.resolve(ctx_ptr->host_path_id);
    auto candidates = workspace.cdb.candidate_entries(host_path);
    if(candidates.empty()) {
        LOG_WARN("fill_header_context_args: host {} has no CDB entry", host_path);
        return false;
    }

    // Apply rules matching the HEADER path (what the user is editing) on top of
    // the host's command — rules are expected to apply uniformly to every file.
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(path, rule_append, rule_remove);
    auto base = pick_host_config(workspace,
                                 host_path,
                                 candidates,
                                 ctx_ptr->host_command_hash,
                                 ctx_ptr->host_base_hash);
    auto applied = workspace.cdb.apply_rules(base, {.remove = rule_remove, .append = rule_append});

    // The header compiles as the host's language, with the header injected
    // as the input; the synthesized preamble lands after the host's own
    // user-content flags (its -include runs first).
    CommandRef ref{path_id,
                   applied,
                   workspace.cdb.input_kind(applied, host_path),
                   CommandSource::IncludeGraph};
    RenderOptions opts;
    if(!ctx_ptr->preamble_path.empty()) {
        opts.preamble = ctx_ptr->preamble_path.c_str();
    }
    directory = workspace.cdb.config(applied).directory;
    arguments = to_strings(workspace.cdb.render(ref, opts));
    if(host_path_id) {
        *host_path_id = ctx_ptr->host_path_id;
    }
    if(out_ref) {
        *out_ref = ref;
    }

    LOG_INFO("resolve_command: header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
    return true;
}

CommandSource ContextResolver::resolve_command(llvm::StringRef path,
                                               std::string& directory,
                                               std::vector<std::string>& arguments,
                                               ContextUse use,
                                               Fid* host_path_id,
                                               llvm::ArrayRef<std::string> extra_prepend,
                                               llvm::ArrayRef<std::string> extra_append,
                                               CommandRef* out_ref) {
    auto path_id = workspace.file_table.intern(path);
    llvm::SmallVector<llvm::StringRef, 3> tried;

    // Fill from the CDB layer with config rules applied (append/remove flags
    // based on file patterns). Also used for tier 4 with the synthesized
    // default config for files without an entry.
    auto fill_from_cdb = [&](CommandSource source) {
        std::vector<std::string> rule_append, rule_remove;
        workspace.config.match_rules(path, rule_append, rule_remove);
        CommandOptions options{.remove = rule_remove,
                               .append = rule_append,
                               .extra_prepend = extra_prepend,
                               .extra_append = extra_append};

        auto candidates = workspace.cdb.candidate_entries(path_id);
        ConfigID base;
        if(candidates.empty()) {
            base = workspace.cdb.fallback_config(path);
        } else {
            base = candidates.front().config;
            // Multi-config projects: honor the user's chosen CDB entry,
            // matched by entry hash so the choice survives CDB reordering.
            const SavedContext* choice = active_choice(use, path_id);
            if(choice && !choice->host_path_id.valid() && !choice->command_hash.empty()) {
                bool base_matched = false;
                if(!choice->base_hash.empty()) {
                    for(auto& candidate: candidates) {
                        if(workspace.cdb.entry_hash_hex(candidate.config) == choice->base_hash) {
                            base = candidate.config;
                            base_matched = true;
                            break;
                        }
                    }
                }
                if(!base_matched) {
                    for(auto& candidate: candidates) {
                        auto applied = workspace.cdb.apply_rules(candidate.config, options);
                        if(workspace.cdb.entry_hash_hex(applied) == choice->command_hash) {
                            base = candidate.config;
                            break;
                        }
                    }
                }
            }
        }

        auto applied = workspace.cdb.apply_rules(base, options);
        CommandRef ref{path_id, applied, workspace.cdb.input_kind(applied, path), source};
        directory = workspace.cdb.config(applied).directory;
        arguments = to_strings(workspace.cdb.render(ref));
        if(out_ref) {
            *out_ref = ref;
        }
    };

    const SavedContext* choice = active_choice(use, path_id);
    bool has_host_choice = choice && choice->host_path_id.valid();

    // 1. If the file has an active header context via switchContext, use the
    //    host source's CDB entry with file path replaced and preamble injected.
    if(has_host_choice) {
        tried.push_back("switch_context");
        if(fill_header_context_args(path,
                                    path_id,
                                    directory,
                                    arguments,
                                    use,
                                    host_path_id,
                                    out_ref)) {
            log_command_decision(path, tried, CommandSource::IncludeGraph, arguments);
            return CommandSource::IncludeGraph;
        }
    }

    // 2. Real CDB entry for the file itself.
    tried.push_back("cdb");
    if(!workspace.cdb.candidate_entries(path_id).empty()) {
        fill_from_cdb(CommandSource::CDBExact);
        log_command_decision(path, tried, CommandSource::CDBExact, arguments);
        return CommandSource::CDBExact;
    }

    // 3. No CDB entry — try automatic header context resolution.
    if(!has_host_choice) {
        tried.push_back("include_graph");
        if(fill_header_context_args(path,
                                    path_id,
                                    directory,
                                    arguments,
                                    use,
                                    host_path_id,
                                    out_ref)) {
            log_command_decision(path, tried, CommandSource::IncludeGraph, arguments);
            return CommandSource::IncludeGraph;
        }
    }

    // 4. Nothing matched — use the synthesized default command, so the file
    //    still compiles and produces diagnostics instead of failing silently.
    tried.push_back("fallback");
    fill_from_cdb(CommandSource::Fallback);
    log_command_decision(path, tried, CommandSource::Fallback, arguments);
    return CommandSource::Fallback;
}

void ContextResolver::append_suffix_include(Fid path_id, std::string& text) {
    auto* context = header_context(path_id);
    if(!context || context->suffix_path.empty()) {
        return;
    }
    if(!text.ends_with('\n')) {
        text += '\n';
    }
    text += "#include \"";
    // Escape like preamble_synthesis's line markers: Windows separators
    // must survive the preprocessor's string literal parsing.
    for(char c: context->suffix_path) {
        if(c == '\\' || c == '"') {
            text += '\\';
        }
        text += c;
    }
    text += "\"\n";
}

std::optional<HeaderContext> ContextResolver::resolve_header_context(Fid header_path_id,
                                                                     ContextUse use,
                                                                     bool synthesize) {
    // Find source files that transitively include this header.
    auto hosts = workspace.dep_graph.find_host_sources(header_path_id);
    if(hosts.empty()) {
        LOG_DEBUG("resolve_header_context: no host sources for path_id={}", header_path_id);
        return std::nullopt;
    }

    // If there's an active context override, prefer that host (and its
    // chosen include occurrence).
    Fid host_path_id;
    std::optional<std::uint32_t> occurrence;
    std::vector<Fid> chain;
    const SavedContext* choice = active_choice(use, header_path_id);
    bool has_host_choice = choice && choice->host_path_id.valid();
    if(has_host_choice) {
        auto preferred = choice->host_path_id;
        auto preferred_path = workspace.file_table.resolve(preferred);
        if(workspace.cdb.has_entry(preferred_path)) {
            auto c = workspace.dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                occurrence = choice->occurrence;
                chain = std::move(c);
            }
        }
    }

    // Fall back to the most relevant host that has a real CDB entry —
    // a host with a synthesized command would just be a fallback in disguise.
    if(chain.empty()) {
        for(auto candidate: workspace.rank_hosts(header_path_id, hosts)) {
            auto candidate_path = workspace.file_table.resolve(candidate);
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
    std::string host_base_hash;
    if(has_host_choice) {
        host_command_hash = choice->command_hash;
        host_base_hash = choice->base_hash;
    }

    if(!synthesize) {
        llvm::SmallVector<Fid> chain_ids(chain.begin(), chain.end() - 1);
        return HeaderContext{host_path_id,
                             "",
                             0,
                             "",
                             occurrence.value_or(0),
                             std::move(host_command_hash),
                             std::move(host_base_hash),
                             std::move(chain_ids),
                             {}};
    }

    // Include directives along the chain are resolved with the host's real
    // search configuration, so same-named headers in different directories
    // cannot be confused.
    auto host_path = workspace.file_table.resolve(host_path_id);
    auto candidates = workspace.cdb.candidate_entries(host_path);
    if(candidates.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(host_path, rule_append, rule_remove);
    auto base =
        pick_host_config(workspace, host_path, candidates, host_command_hash, host_base_hash);
    auto applied = workspace.cdb.apply_rules(base, {.remove = rule_remove, .append = rule_append});
    CommandRef host_ref{host_path_id,
                        applied,
                        workspace.cdb.input_kind(applied, host_path),
                        CommandSource::CDBExact};

    auto search_config = workspace.cdb.search_config(host_ref);
    DirListingCache dir_cache;
    dir_cache.shared = &workspace.file_table;
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
        // Normalize through the file table: resolve_include builds native
        // separators, but chain paths compared against it are table-normalized.
        return std::string(workspace.file_table.resolve(workspace.file_table.intern(result->path)));
    };

    // Read the chain files (all but the target) from disk. The synthesized
    // preamble deliberately reflects disk state, never open-document buffers:
    // open files must not be depended upon by other files. The hash covers
    // the bytes just read — the bytes the synthesized preamble embeds —
    // and the paired stat becomes the version's fast path only when the
    // read proved it reliable (see read_file_observed); otherwise the next
    // check compares the disk against the embedded bytes.
    std::vector<std::string> chain_contents;
    llvm::SmallVector<ChainEntry> chain_entries;
    DepsSnapshot deps;
    chain_contents.reserve(chain.size() - 1);
    chain_entries.reserve(chain.size() - 1);
    deps.reserve(chain.size());
    for(std::size_t i = 0; i + 1 < chain.size(); ++i) {
        auto cur_path = workspace.file_table.resolve(chain[i]);
        auto observed = read_file_observed(cur_path.data());
        if(!observed) {
            LOG_WARN("resolve_header_context: cannot read {}", cur_path);
            return std::nullopt;
        }
        chain_contents.emplace_back(observed->content->getBuffer());
        chain_entries.push_back({cur_path, chain_contents.back()});
        workspace.file_table.observe(chain[i], observed->obs);
        auto vid = workspace.file_table.intern_version(chain[i], observed->obs.hash);
        deps.push_back({.path_id = chain[i], .version = vid});
        workspace.file_table.try_stamp(vid,
                                       observed->obs.size,
                                       observed->obs.mtime_ns,
                                       observed->obs.uid_device,
                                       observed->obs.uid_file);
    }

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the prefix/suffix must point at a copy.
    auto target_path = workspace.file_table.resolve(chain.back());
    std::string self_snapshot_path;
    std::optional<ObservedFile> target_observed;
    auto preamble_dir = path::join(workspace.config.project.cache_dir, "header_context");
    if((target_observed = read_file_observed(target_path.data()))) {
        auto content = target_observed->content->getBuffer();
        workspace.file_table.observe(chain.back(), target_observed->obs);
        self_snapshot_path =
            path::join(preamble_dir, std::format("{:016x}.self.h", target_observed->obs.hash));
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
        record_synthesized_host(self_snapshot_path, host_path_id);
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
    record_synthesized_host(preamble_path, host_path_id);

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
        record_synthesized_host(suffix_path, host_path_id);
    }

    // The chain files' snapshot (`deps`) was recorded as they were read:
    // their content lives inside the synthesized preamble, so clang's own
    // dependency tracking never sees them.
    llvm::SmallVector<Fid> chain_ids(chain.begin(), chain.end() - 1);
    if(!self_snapshot_path.empty()) {
        // The self-snapshot mirrors the header's disk state; re-synthesize
        // when it changes so other-occurrence expansions stay current.
        auto vid = workspace.file_table.intern_version(chain.back(), target_observed->obs.hash);
        deps.push_back({.path_id = chain.back(), .version = vid});
        workspace.file_table.try_stamp(vid,
                                       target_observed->obs.size,
                                       target_observed->obs.mtime_ns,
                                       target_observed->obs.uid_device,
                                       target_observed->obs.uid_file);
    }

    return HeaderContext{host_path_id,
                         preamble_path,
                         preamble_hash,
                         std::move(suffix_path),
                         occurrence.value_or(0),
                         std::move(host_command_hash),
                         std::move(host_base_hash),
                         std::move(chain_ids),
                         std::move(deps)};
}

bool ContextResolver::pin_alive(llvm::StringRef entry_path, const SavedContext& saved) const {
    std::vector<std::string> rule_append, rule_remove;
    workspace.config.match_rules(entry_path, rule_append, rule_remove);
    for(auto& entry: workspace.cdb.candidate_entries(entry_path)) {
        if(!saved.base_hash.empty() &&
           workspace.cdb.entry_hash_hex(entry.config) == saved.base_hash) {
            return true;
        }
        auto applied =
            workspace.cdb.apply_rules(entry.config, {.remove = rule_remove, .append = rule_append});
        if(workspace.cdb.entry_hash_hex(applied) == saved.command_hash) {
            return true;
        }
    }
    return false;
}

void ContextResolver::validate_saved_context(Fid path_id) {
    auto path = workspace.file_table.resolve(path_id);

    // A context choice persisted from an earlier session stays authoritative
    // only if it still holds: the CDB or include graph may have changed
    // while the server was down, and a stale choice suppresses automatic
    // host resolution and strands the file on the fallback command.
    if(auto it = saved_contexts.find(path_id); it != saved_contexts.end()) {
        auto& ws = workspace;
        auto& saved = it->second;

        bool valid = false;
        if(saved.host_path_id.valid()) {
            auto host_path = ws.file_table.resolve(saved.host_path_id);
            valid = ws.cdb.has_entry(host_path) &&
                    !ws.dep_graph.find_include_chain(saved.host_path_id, path_id).empty() &&
                    (saved.command_hash.empty() || pin_alive(host_path, saved));
        } else if(!saved.command_hash.empty()) {
            valid = ws.cdb.has_entry(path) && pin_alive(path, saved);
        }
        if(!valid) {
            LOG_INFO("didOpen: dropping stale saved context for {}", path);
            saved_contexts.erase(it);
            // The drop must reach the contexts blob, or the stale choice
            // resurrects from disk at the next start.
            workspace.mark_contexts_dirty();
        }
    }
}

}  // namespace clice
