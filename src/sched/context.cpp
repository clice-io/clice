#include "sched/context.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "sched/hosting.h"
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

/// Pick the candidate matching a pinned command (multi-configuration files
/// and hosts), defaulting to the build's first command. `paths` are the
/// files whose edits the published hash was computed with.
static Candidate pick_pinned_config(Workspace& workspace,
                                    Fid file,
                                    llvm::ArrayRef<Candidate> candidates,
                                    llvm::ArrayRef<llvm::StringRef> paths,
                                    llvm::StringRef language_path,
                                    llvm::StringRef pinned_hash,
                                    llvm::StringRef pinned_base) {
    // The base identity resolved at pin time is exact; the applied hash
    // remains as the fallback for pins saved before the base was recorded
    // (and cannot distinguish candidates the rules collapse together).
    if(!pinned_base.empty()) {
        for(auto& entry: candidates) {
            if(workspace.cdb.entry_hash_hex(entry.config) == pinned_base) {
                return entry;
            }
        }
    }
    if(!pinned_hash.empty()) {
        for(auto& entry: candidates) {
            auto ref =
                workspace.build.resolve(file, entry.config, entry.source, paths, language_path);
            if(workspace.cdb.entry_hash_hex(ref.config) == pinned_hash) {
                return entry;
            }
        }
    }
    return candidates.front();
}

HeaderMode ContextResolver::header_mode(llvm::StringRef path, Fid path_id) const {
    if(path.ends_with(".def") || path.ends_with(".inc") || path.ends_with(".inl") ||
       path.ends_with(".tpp") || path.ends_with(".ipp")) {
        return HeaderMode::NeedsContext;
    }
    if(auto it = header_verdicts.find(path_id); it != header_verdicts.end()) {
        return it->second.mode;
    }
    return HeaderMode::Unknown;
}

void ContextResolver::forget_self_contained(Fid path_id) {
    if(auto it = header_verdicts.find(path_id);
       it != header_verdicts.end() && it->second.mode == HeaderMode::SelfContained) {
        header_verdicts.erase(it);
    }
}

std::uint64_t ContextResolver::persisted_mode_hash(Fid path_id) const {
    auto it = header_verdicts.find(path_id);
    if(it == header_verdicts.end() || it->second.mode != HeaderMode::NeedsContext) {
        return 0;
    }
    return it->second.content_hash;
}

void ContextResolver::record_header_mode(Fid path_id, HeaderMode mode, std::uint64_t content_hash) {
    auto persisted = persisted_mode_hash(path_id);
    header_verdicts[path_id] = {
        .mode = mode,
        .content_hash = mode == HeaderMode::NeedsContext ? content_hash : 0,
    };
    if(persisted_mode_hash(path_id) != persisted) {
        workspace.mark_artifacts_dirty();
    }
}

void ContextResolver::reset_header_mode(Fid path_id) {
    if(persisted_mode_hash(path_id) != 0) {
        workspace.mark_artifacts_dirty();
    }
    header_verdicts.erase(path_id);
}

void ContextResolver::dump_mode_slices(std::vector<CacheModeEntry>& modes,
                                       llvm::function_ref<std::uint32_t(Fid)> intern_id) const {
    for(auto& [path_id, verdict]: header_verdicts) {
        // A verdict scored with no disk observation (hash 0) cannot be
        // validated on load, so it stays in memory: persisted, it would
        // skip the self-containment trial for whatever bytes the next
        // session finds on disk.
        if(verdict.mode != HeaderMode::NeedsContext || verdict.content_hash == 0)
            continue;
        modes.push_back(
            {intern_id(path_id), static_cast<std::uint32_t>(verdict.mode), verdict.content_hash});
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

    for(auto& [path_id, saved]: selections) {
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
        header_verdicts[id] = {.mode = HeaderMode::NeedsContext,
                               .content_hash = entry.content_hash};
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
        Selection saved;
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
        selections[workspace.file_table.intern(file)] = std::move(saved);
    }

    for(auto& entry: artifacts) {
        auto file = resolve(entry.file);
        auto host = resolve(entry.host);
        if(file.empty() || host.empty())
            continue;
        // A file the store evicted since (or a wiped cache) has nothing
        // left to open under the host's command; the record leaves the
        // blob with the next save.
        if(!llvm::sys::fs::exists(file)) {
            workspace.mark_contexts_dirty();
            continue;
        }
        synthesized_hosts[file] = workspace.file_table.intern(host);
    }
}

/// Whether the store still serves a synthesized file: a blob the budget
/// evicted, or a directory wiped from outside, must not reach a compile
/// command. The lookup also refreshes the blob's last use, so a context
/// in service never ages out under the budget.
static bool artifact_alive(Workspace& workspace, llvm::StringRef path) {
    if(path.empty()) {
        return true;
    }
    auto served = workspace.store->lookup(header_context_ns, llvm::sys::path::stem(path));
    return served && llvm::sys::fs::exists(*served);
}

static bool artifacts_alive(Workspace& workspace, const HeaderContext& context) {
    return artifact_alive(workspace, context.preamble_path) &&
           artifact_alive(workspace, context.suffix_path) &&
           artifact_alive(workspace, context.snapshot_path);
}

void ContextResolver::drop_evicted_artifacts() {
    llvm::SmallVector<std::string> gone;
    for(auto& entry: synthesized_hosts) {
        if(!llvm::sys::fs::exists(entry.getKey())) {
            gone.push_back(entry.getKey().str());
        }
    }
    for(auto& path: gone) {
        synthesized_hosts.erase(path);
    }
    if(!gone.empty()) {
        workspace.mark_contexts_dirty();
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
        auto commands = workspace.build.commands(it->second);
        if(commands.empty()) {
            return false;
        }
        // The artifact is a fragment of the host TU: it compiles as the
        // host's language, under the host's effective command, with the
        // artifact path injected as the input.
        auto ref = workspace.build.resolve(path_id,
                                           commands.front().config,
                                           CommandSource::IncludeGraph,
                                           host_path,
                                           host_path);
        directory = workspace.cdb.config(ref.config).directory;
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
    const Selection* choice = selection(use, path_id);
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
            if(override_mismatch || mode_mismatch || !artifacts_alive(workspace, *cached) ||
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
    auto commands = host_commands(workspace, path_id, ctx_ptr->host_path_id);
    if(commands.empty()) {
        LOG_WARN("fill_header_context_args: host {} has no compile command", host_path);
        return false;
    }

    // The header inherits the host's world: the rules matching the host
    // and the rules matching the header both edit the borrowed command,
    // each once, in declaration order.
    llvm::StringRef edit_paths[] = {host_path, path};
    auto base = pick_pinned_config(workspace,
                                   path_id,
                                   commands,
                                   edit_paths,
                                   host_path,
                                   ctx_ptr->host_command_hash,
                                   ctx_ptr->host_base_hash)
                    .config;

    // The header compiles as the host's language, with the header injected
    // as the input; the synthesized preamble lands after the host's own
    // user-content flags (its -include runs first).
    auto ref =
        workspace.build.resolve(path_id, base, CommandSource::IncludeGraph, edit_paths, host_path);
    RenderOptions opts;
    if(!ctx_ptr->preamble_path.empty()) {
        opts.preamble = ctx_ptr->preamble_path.c_str();
    }
    directory = workspace.cdb.config(ref.config).directory;
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
    llvm::SmallVector<llvm::StringRef, 4> tried;

    // Render `base` with the rule edits of `paths` applied: an entry, a
    // default command, a borrowed or the builtin one alike.
    auto fill = [&](ConfigID base,
                    CommandSource source,
                    llvm::ArrayRef<llvm::StringRef> paths,
                    llvm::StringRef language_path) {
        auto ref =
            workspace.build
                .resolve(path_id, base, source, paths, language_path, extra_prepend, extra_append);
        directory = workspace.cdb.config(ref.config).directory;
        arguments = to_strings(workspace.cdb.render(ref));
        if(out_ref) {
            *out_ref = ref;
        }
    };

    auto fill_from_cdb = [&](llvm::ArrayRef<Candidate> candidates) -> CommandSource {
        // Multi-config projects: honor the user's chosen entry, matched by
        // entry hash so the choice survives reordering.
        llvm::StringRef pinned_hash, pinned_base;
        const Selection* choice = selection(use, path_id);
        if(choice && !choice->host_path_id.valid()) {
            pinned_hash = choice->command_hash;
            pinned_base = choice->base_hash;
        }
        auto picked = pick_pinned_config(workspace,
                                         path_id,
                                         candidates,
                                         path,
                                         path,
                                         pinned_hash,
                                         pinned_base);
        fill(picked.config, picked.source, path, path);
        return picked.source;
    };

    const Selection* choice = selection(use, path_id);
    bool has_host_choice = choice && choice->host_path_id.valid();
    guessed_commands.erase(path_id);

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

    // 2. The file's own command: a database entry, or the default command
    //    of a source the build compiles as a unit. A header's default
    //    command is only the last resort below, after host inference.
    tried.push_back("cdb");
    auto commands = workspace.build.commands(path_id);
    if(workspace.build.unit(path_id)) {
        auto source = fill_from_cdb(commands);
        log_command_decision(path, tried, source, arguments);
        return source;
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

    // 4. A rule's default command for a file the build does not compile as
    //    a unit (a header under a default-command rule).
    if(!commands.empty()) {
        tried.push_back("default");
        fill(commands.front().config, CommandSource::Default, path, path);
        log_command_decision(path, tried, CommandSource::Default, arguments);
        return CommandSource::Default;
    }

    // 5. A nearby unit's command: the file compiles as that unit's
    //    language, under its command edited for both files.
    tried.push_back("inferred");
    guessed_commands.insert(path_id);
    if(auto lender = command_lender(workspace, path_id)) {
        auto lender_path = workspace.file_table.resolve(lender->unit);
        llvm::StringRef edit_paths[] = {path, lender_path};
        fill(lender->config, CommandSource::Inferred, edit_paths, lender_path);
        LOG_INFO("resolve_command: {} borrows the command of {}", path, lender_path);
        log_command_decision(path, tried, CommandSource::Inferred, arguments);
        return CommandSource::Inferred;
    }

    // 6. The builtin fallback, so the file still compiles and produces
    //    diagnostics instead of failing silently.
    tried.push_back("fallback");
    fill(workspace.build.builtin(path), CommandSource::Fallback, path, path);
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
    // A pinned host (and its chosen include occurrence) wins while it
    // still compiles and still includes the header; otherwise the build's
    // default host.
    Fid host_path_id;
    std::optional<std::uint32_t> occurrence;
    std::vector<Fid> chain;
    const Selection* choice = selection(use, header_path_id);
    bool has_host_choice = choice && choice->host_path_id.valid();
    if(has_host_choice) {
        auto preferred = choice->host_path_id;
        if(!workspace.build.commands(preferred).empty()) {
            auto c = workspace.dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                occurrence = choice->occurrence;
                chain = std::move(c);
            }
        }
    }
    if(chain.empty()) {
        auto host = default_host(workspace, header_path_id);
        if(!host) {
            LOG_DEBUG("resolve_header_context: no host for path_id={}", header_path_id);
            return std::nullopt;
        }
        host_path_id = host->file;
        chain = std::move(host->chain);
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
    auto commands = host_commands(workspace, chain.back(), host_path_id);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto target_path = workspace.file_table.resolve(chain.back());
    llvm::StringRef edit_paths[] = {host_path, target_path};
    auto picked = pick_pinned_config(workspace,
                                     host_path_id,
                                     commands,
                                     edit_paths,
                                     host_path,
                                     host_command_hash,
                                     host_base_hash);
    auto host_ref =
        workspace.build.resolve(host_path_id, picked.config, picked.source, edit_paths, host_path);

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

    if(!workspace.store) {
        LOG_WARN("resolve_header_context: no cache store to hold the preamble of {}",
                 workspace.file_table.resolve(chain.back()));
        return std::nullopt;
    }
    // Every synthesized file is a content-addressed blob of the store: the
    // same bytes land on the same path across sessions, so a reopened
    // header finds its preamble — and the PCH keyed on that path — intact,
    // and the store's budget bounds what a long-lived cache accumulates.
    auto store_blob = [&](std::string key, llvm::StringRef content) -> std::optional<std::string> {
        // A hit is the manifest's word; the file may have been wiped from
        // outside (the store survives that), so a missing one is
        // republished under its key.
        if(auto hit = workspace.store->lookup(header_context_ns, key);
           hit && llvm::sys::fs::exists(*hit)) {
            return hit;
        }
        auto pending = workspace.store->begin_store(header_context_ns, key);
        if(auto result = fs::write(pending.tmp_path, content); !result) {
            LOG_WARN("resolve_header_context: cannot write {}: {}",
                     pending.tmp_path,
                     result.error().message());
            return std::nullopt;
        }
        auto published = workspace.store->commit(std::move(pending));
        if(!published) {
            LOG_WARN("resolve_header_context: cannot publish {}: {}",
                     key,
                     published.error().message());
            return std::nullopt;
        }
        LOG_INFO("resolve_header_context: stored {} for header path_id={}",
                 *published,
                 header_path_id);
        return *published;
    };

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the prefix/suffix must point at a copy.
    std::string self_snapshot_path;
    std::optional<ObservedFile> target_observed;
    if((target_observed = read_file_observed(target_path.data()))) {
        auto content = target_observed->content->getBuffer();
        workspace.file_table.observe(chain.back(), target_observed->obs);
        auto stored = store_blob(std::format("{:016x}.self", target_observed->obs.hash), content);
        if(!stored) {
            return std::nullopt;
        }
        self_snapshot_path = std::move(*stored);
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

    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(preamble));
    auto stored_preamble = store_blob(std::format("{:016x}", preamble_hash), preamble);
    if(!stored_preamble) {
        return std::nullopt;
    }
    auto preamble_path = std::move(*stored_preamble);
    record_synthesized_host(preamble_path, host_path_id);

    // The suffix restores everything after the include position (closing
    // braces of enums/functions the fragment is embedded in). Injected by
    // appending one #include line to the header's buffer at compile time.
    std::string suffix_path;
    if(!synthesized->suffix.empty()) {
        auto suffix_hash = llvm::xxh3_64bits(llvm::StringRef(synthesized->suffix));
        auto stored = store_blob(std::format("{:016x}.suffix", suffix_hash), synthesized->suffix);
        if(!stored) {
            return std::nullopt;
        }
        suffix_path = std::move(*stored);
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
                         std::move(self_snapshot_path),
                         occurrence.value_or(0),
                         std::move(host_command_hash),
                         std::move(host_base_hash),
                         std::move(chain_ids),
                         std::move(deps)};
}

bool ContextResolver::pin_alive(Fid entry_file,
                                llvm::ArrayRef<llvm::StringRef> paths,
                                const Selection& saved) const {
    auto entry_path = workspace.file_table.resolve(entry_file);
    for(auto& entry: workspace.build.commands(entry_file)) {
        if(!saved.base_hash.empty() &&
           workspace.cdb.entry_hash_hex(entry.config) == saved.base_hash) {
            return true;
        }
        auto ref =
            workspace.build.resolve(entry_file, entry.config, entry.source, paths, entry_path);
        if(workspace.cdb.entry_hash_hex(ref.config) == saved.command_hash) {
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
    if(auto it = selections.find(path_id); it != selections.end()) {
        auto& ws = workspace;
        auto& saved = it->second;

        bool valid = false;
        if(saved.host_path_id.valid()) {
            auto host_path = ws.file_table.resolve(saved.host_path_id);
            llvm::StringRef edit_paths[] = {host_path, path};
            valid =
                !ws.build.commands(saved.host_path_id).empty() &&
                !ws.dep_graph.find_include_chain(saved.host_path_id, path_id).empty() &&
                (saved.command_hash.empty() || pin_alive(saved.host_path_id, edit_paths, saved));
        } else if(!saved.command_hash.empty()) {
            valid = !ws.build.commands(path_id).empty() && pin_alive(path_id, path, saved);
        }
        if(!valid) {
            LOG_INFO("didOpen: dropping stale saved context for {}", path);
            selections.erase(it);
            // The drop must reach the contexts blob, or the stale choice
            // resurrects from disk at the next start.
            workspace.mark_contexts_dirty();
        }
    }
}

}  // namespace clice
