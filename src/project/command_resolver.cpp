#include "project/command_resolver.h"

#include <format>
#include <optional>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "project/hosting.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/preamble_synthesis.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
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
static Candidate pick_pinned_config(Project& project,
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
            if(project.cdb.entry_hash_hex(entry.config) == pinned_base) {
                return entry;
            }
        }
    }
    if(!pinned_hash.empty()) {
        for(auto& entry: candidates) {
            auto ref =
                project.build.resolve(file, entry.config, entry.source, paths, language_path);
            if(project.cdb.entry_hash_hex(ref.config) == pinned_hash) {
                return entry;
            }
        }
    }
    return candidates.front();
}

HeaderMode CommandResolver::header_mode(llvm::StringRef path, Fid path_id) const {
    if(is_context_header_path(path)) {
        return HeaderMode::NeedsContext;
    }
    if(auto it = header_verdicts.find(path_id); it != header_verdicts.end()) {
        return it->second.mode;
    }
    return HeaderMode::Unknown;
}

void CommandResolver::forget_self_contained(Fid path_id) {
    if(auto it = header_verdicts.find(path_id);
       it != header_verdicts.end() && it->second.mode == HeaderMode::SelfContained) {
        header_verdicts.erase(it);
    }
}

std::uint64_t CommandResolver::persisted_mode_hash(Fid path_id) const {
    auto it = header_verdicts.find(path_id);
    if(it == header_verdicts.end() || it->second.mode != HeaderMode::NeedsContext) {
        return 0;
    }
    return it->second.content_hash;
}

void CommandResolver::record_header_mode(Fid path_id, HeaderMode mode, std::uint64_t content_hash) {
    auto persisted = persisted_mode_hash(path_id);
    header_verdicts[path_id] = {
        .mode = mode,
        .content_hash = mode == HeaderMode::NeedsContext ? content_hash : 0,
    };
    if(persisted_mode_hash(path_id) != persisted) {
        project.mark_artifacts_dirty();
    }
}

void CommandResolver::reset_header_mode(Fid path_id) {
    if(persisted_mode_hash(path_id) != 0) {
        project.mark_artifacts_dirty();
    }
    header_verdicts.erase(path_id);
}

void CommandResolver::dump_mode_slices(std::vector<CacheModeEntry>& modes,
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

void CommandResolver::load_mode_slices(llvm::ArrayRef<CacheModeEntry> modes,
                                       llvm::function_ref<llvm::StringRef(std::uint32_t)> resolve) {
    for(auto& entry: modes) {
        auto file = resolve(entry.file);
        // The writer never emits unbound (hash 0) verdicts; an entry
        // carrying one is corrupt and must not bypass the content gate.
        if(file.empty() || entry.content_hash == 0 ||
           static_cast<HeaderMode>(entry.mode) != HeaderMode::NeedsContext)
            continue;
        auto id = project.file_table.intern(file);
        // The verdict is tied to the header's contents — a file edited
        // while the server was down must re-earn its trial.
        auto disk = project.file_table.current(id);
        if(!disk || disk->hash != entry.content_hash)
            continue;
        header_verdicts[id] = {.mode = HeaderMode::NeedsContext,
                               .content_hash = entry.content_hash};
    }
}

/// Whether the store still serves a synthesized file: a blob the budget
/// evicted, or a directory wiped from outside, must not reach a compile
/// command. The lookup also refreshes the blob's last use, so a context
/// in service never ages out under the budget.
static bool artifact_alive(Project& project, llvm::StringRef path) {
    if(path.empty()) {
        return true;
    }
    auto served = project.store->lookup(header_context_ns, llvm::sys::path::stem(path));
    return served && llvm::sys::fs::exists(*served);
}

static bool artifacts_alive(Project& project, const HeaderContext& context) {
    return artifact_alive(project, context.preamble_path) &&
           artifact_alive(project, context.suffix_path) &&
           artifact_alive(project, context.snapshot_path);
}

bool CommandResolver::fill_header_context_args(llvm::StringRef path,
                                               Fid path_id,
                                               std::string& directory,
                                               std::vector<std::string>& arguments,
                                               const CommandRequest& request,
                                               Resolution& resolution) {
    // Opening one of our own synthesized files (prefix/suffix/snapshot):
    // it is a fragment of the host TU it was synthesized for, so compile
    // it with that host's command, treated as self-contained. It must not
    // derive context from other synthesized state; without a recorded
    // host (e.g. a stale artifact from a wiped cache), fall through to
    // the default command.
    if(project.is_synthesized_artifact(path)) {
        if(!request.synthesized_hosts) {
            return false;
        }
        auto it = request.synthesized_hosts->find(path);
        if(it == request.synthesized_hosts->end()) {
            return false;
        }
        auto host_path = project.file_table.resolve(it->second);
        auto commands = project.build.commands(it->second);
        if(commands.empty()) {
            return false;
        }
        // The artifact is a fragment of the host TU: it compiles as the
        // host's language, under the host's effective command, with the
        // artifact path injected as the input.
        auto ref = project.build.resolve(path_id,
                                         commands.front().config,
                                         CommandSource::IncludeGraph,
                                         host_path,
                                         host_path);
        directory = project.cdb.config(ref.config).directory;
        arguments = to_strings(project.cdb.render(ref));
        resolution.host = it->second;
        resolution.ref = ref;
        return true;
    }

    // Self-containment routing: an Unknown or SelfContained header borrows
    // the host command without a prefix; NeedsContext synthesizes one.
    // run_compile() flips Unknown to NeedsContext when the trial compile's
    // diagnostics indicate missing includer state. An explicitly chosen
    // occurrence — even #0 — only has meaning under includer-context
    // semantics, so it forces synthesis regardless of the verdict.
    const Selection* choice = request.selection;
    bool has_host_choice = choice && choice->host_path_id.valid();
    // A synthesized context is a store artifact; a read-only reader (the
    // query command) compiles the header under its host's command instead.
    bool can_synthesize = project.store && !project.store->read_only();
    bool synthesize = can_synthesize && (header_mode(path, path_id) == HeaderMode::NeedsContext ||
                                         (has_host_choice && choice->occurrence.has_value()));

    // Use cached context if it is still valid; otherwise resolve. The cache
    // is dropped when an active context override points to a different host
    // or include occurrence, when the routing mode changed, or when any
    // chain file changed on disk (the synthesized preamble embeds their
    // content, so it must be rebuilt). Only editor-facing compiles consult
    // the cache; background indexing must stay independent of per-editor
    // context state, so it resolves fresh every time.
    auto* cache = request.header_contexts;
    const HeaderContext* ctx_ptr = nullptr;
    if(cache) {
        if(auto cached = cache->find(path_id); cached != cache->end()) {
            auto& context = cached->second;
            bool override_mismatch =
                has_host_choice && (context.host_path_id != choice->host_path_id ||
                                    context.occurrence != choice->occurrence.value_or(0) ||
                                    context.host_command_hash != choice->command_hash ||
                                    context.host_base_hash != choice->base_hash);
            bool mode_mismatch = context.preamble_path.empty() == synthesize;
            auto wave = project.file_table.wave();
            if(override_mismatch || mode_mismatch || !artifacts_alive(project, context) ||
               deps_changed(project.file_table, context.deps)) {
                cache->erase(cached);
            } else {
                ctx_ptr = &context;
            }
        }
    }

    std::optional<HeaderContext> local_ctx;
    if(!ctx_ptr) {
        auto resolved = resolve_header_context(path_id, choice, synthesize, resolution.synthesized);
        if(!resolved) {
            LOG_WARN("No CDB entry and no header context for {}", path);
            return false;
        }
        if(cache) {
            ctx_ptr = &((*cache)[path_id] = std::move(*resolved));
        } else {
            local_ctx = std::move(*resolved);
            ctx_ptr = &*local_ctx;
        }
    }

    auto host_path = project.file_table.resolve(ctx_ptr->host_path_id);
    auto commands = host_commands(project, path_id, ctx_ptr->host_path_id);
    if(commands.empty()) {
        LOG_WARN("fill_header_context_args: host {} has no compile command", host_path);
        return false;
    }

    // The header inherits the host's world: the rules matching the host
    // and the rules matching the header both edit the borrowed command,
    // each once, in declaration order.
    llvm::StringRef edit_paths[] = {host_path, path};
    auto base = pick_pinned_config(project,
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
        project.build.resolve(path_id, base, CommandSource::IncludeGraph, edit_paths, host_path);
    RenderOptions opts;
    if(!ctx_ptr->preamble_path.empty()) {
        opts.preamble = ctx_ptr->preamble_path.c_str();
    }
    directory = project.cdb.config(ref.config).directory;
    arguments = to_strings(project.cdb.render(ref, opts));
    resolution.host = ctx_ptr->host_path_id;
    resolution.ref = ref;

    LOG_INFO("resolve_command: header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
    return true;
}

Resolution CommandResolver::resolve_command(llvm::StringRef path,
                                            std::string& directory,
                                            std::vector<std::string>& arguments,
                                            const CommandRequest& request) {
    auto path_id = project.file_table.intern(path);
    llvm::SmallVector<llvm::StringRef, 4> tried;
    Resolution resolution;

    // Render `base` with the rule edits of `paths` applied: an entry, a
    // default command, a borrowed or the builtin one alike.
    auto fill = [&](ConfigID base,
                    CommandSource source,
                    llvm::ArrayRef<llvm::StringRef> paths,
                    llvm::StringRef language_path) {
        auto ref = project.build.resolve(path_id,
                                         base,
                                         source,
                                         paths,
                                         language_path,
                                         request.extra_prepend,
                                         request.extra_append);
        directory = project.cdb.config(ref.config).directory;
        arguments = to_strings(project.cdb.render(ref));
        resolution.ref = ref;
    };

    auto settle = [&](CommandSource source) {
        resolution.source = source;
        log_command_decision(path, tried, source, arguments);
        return std::move(resolution);
    };

    const Selection* choice = request.selection;
    bool has_host_choice = choice && choice->host_path_id.valid();

    // 1. If the file has an active header context via switchContext, use the
    //    host source's CDB entry with file path replaced and preamble injected.
    if(has_host_choice) {
        tried.push_back("switch_context");
        if(fill_header_context_args(path, path_id, directory, arguments, request, resolution)) {
            return settle(CommandSource::IncludeGraph);
        }
    }

    // 2. The file's own command: a database entry, or the default command
    //    of a source the build compiles as a unit. A header's default
    //    command is only the last resort below, after host inference.
    //    Multi-config projects honor the user's chosen entry, matched by
    //    entry hash so the choice survives reordering.
    tried.push_back("cdb");
    auto commands = project.build.commands(path_id);
    if(project.build.unit(path_id)) {
        llvm::StringRef pinned_hash, pinned_base;
        if(choice && !has_host_choice) {
            pinned_hash = choice->command_hash;
            pinned_base = choice->base_hash;
        }
        auto picked =
            pick_pinned_config(project, path_id, commands, path, path, pinned_hash, pinned_base);
        fill(picked.config, picked.source, path, path);
        return settle(picked.source);
    }

    // 3. No CDB entry — try automatic header context resolution.
    if(!has_host_choice) {
        tried.push_back("include_graph");
        if(fill_header_context_args(path, path_id, directory, arguments, request, resolution)) {
            return settle(CommandSource::IncludeGraph);
        }
    }

    // 4. A rule's default command for a file the build does not compile as
    //    a unit (a header under a default-command rule).
    if(!commands.empty()) {
        tried.push_back("default");
        fill(commands.front().config, CommandSource::Default, path, path);
        return settle(CommandSource::Default);
    }

    // 5. A nearby unit's command: the file compiles as that unit's
    //    language, under its command edited for both files.
    tried.push_back("inferred");
    if(auto lender = command_lender(project, path_id)) {
        auto lender_path = project.file_table.resolve(lender->unit);
        llvm::StringRef edit_paths[] = {path, lender_path};
        fill(lender->config, CommandSource::Inferred, edit_paths, lender_path);
        LOG_INFO("resolve_command: {} borrows the command of {}", path, lender_path);
        return settle(CommandSource::Inferred);
    }

    // 6. The builtin fallback, so the file still compiles and produces
    //    diagnostics instead of failing silently.
    tried.push_back("fallback");
    fill(project.build.builtin(path), CommandSource::Fallback, path, path);
    return settle(CommandSource::Fallback);
}

std::optional<HeaderContext>
    CommandResolver::resolve_header_context(Fid header_path_id,
                                            const Selection* choice,
                                            bool synthesize,
                                            llvm::SmallVectorImpl<std::string>& synthesized_files) {
    // A pinned host (and its chosen include occurrence) wins while it
    // still compiles and still includes the header; otherwise the build's
    // default host.
    Fid host_path_id;
    std::optional<std::uint32_t> occurrence;
    std::vector<Fid> chain;
    bool has_host_choice = choice && choice->host_path_id.valid();
    if(has_host_choice) {
        auto preferred = choice->host_path_id;
        if(!project.build.commands(preferred).empty()) {
            auto c = project.dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                occurrence = choice->occurrence;
                chain = std::move(c);
            }
        }
    }
    if(chain.empty()) {
        auto host = default_host(project, header_path_id);
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
    auto host_path = project.file_table.resolve(host_path_id);
    auto commands = host_commands(project, chain.back(), host_path_id);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto target_path = project.file_table.resolve(chain.back());
    llvm::StringRef edit_paths[] = {host_path, target_path};
    auto picked = pick_pinned_config(project,
                                     host_path_id,
                                     commands,
                                     edit_paths,
                                     host_path,
                                     host_command_hash,
                                     host_base_hash);
    auto host_ref =
        project.build.resolve(host_path_id, picked.config, picked.source, edit_paths, host_path);

    auto search_config = project.cdb.search_config(host_ref);
    DirListingCache dir_cache;
    dir_cache.shared = &project.file_table;
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
        return std::string(project.file_table.resolve(project.file_table.intern(result->path)));
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
        auto cur_path = project.file_table.resolve(chain[i]);
        auto observed = read_file_observed(cur_path.data());
        if(!observed) {
            LOG_WARN("resolve_header_context: cannot read {}", cur_path);
            return std::nullopt;
        }
        chain_contents.emplace_back(observed->content->getBuffer());
        chain_entries.push_back({cur_path, chain_contents.back()});
        project.file_table.observe(chain[i], observed->obs);
        auto vid = project.file_table.intern_version(chain[i], observed->obs.hash);
        deps.push_back({.path_id = chain[i], .version = vid});
        project.file_table.try_stamp(vid,
                                     observed->obs.size,
                                     observed->obs.mtime_ns,
                                     observed->obs.uid_device,
                                     observed->obs.uid_file);
    }

    if(!project.store) {
        LOG_WARN("resolve_header_context: no cache store to hold the preamble of {}",
                 project.file_table.resolve(chain.back()));
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
        if(auto hit = project.store->lookup(header_context_ns, key);
           hit && llvm::sys::fs::exists(*hit)) {
            return hit;
        }
        auto pending = project.store->begin_store(header_context_ns, key);
        if(auto result = fs::write(pending.tmp_path, content); !result) {
            LOG_WARN("resolve_header_context: cannot write {}: {}",
                     pending.tmp_path,
                     result.error().message());
            return std::nullopt;
        }
        auto published = project.store->commit(std::move(pending));
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
        project.file_table.observe(chain.back(), target_observed->obs);
        auto stored = store_blob(std::format("{:016x}.self", target_observed->obs.hash), content);
        if(!stored) {
            return std::nullopt;
        }
        self_snapshot_path = std::move(*stored);
        synthesized_files.push_back(self_snapshot_path);
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
    synthesized_files.push_back(preamble_path);

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
        synthesized_files.push_back(suffix_path);
    }

    // The chain files' snapshot (`deps`) was recorded as they were read:
    // their content lives inside the synthesized preamble, so clang's own
    // dependency tracking never sees them.
    llvm::SmallVector<Fid> chain_ids(chain.begin(), chain.end() - 1);
    if(!self_snapshot_path.empty()) {
        // The self-snapshot mirrors the header's disk state; re-synthesize
        // when it changes so other-occurrence expansions stay current.
        auto vid = project.file_table.intern_version(chain.back(), target_observed->obs.hash);
        deps.push_back({.path_id = chain.back(), .version = vid});
        project.file_table.try_stamp(vid,
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

}  // namespace clice
