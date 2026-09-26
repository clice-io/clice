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
                                    llvm::ArrayRef<CanonicalRef> paths,
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

HeaderMode CommandResolver::header_mode(Fid path_id) const {
    if(is_context_header_path(project.file_table.resolve(path_id))) {
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
        auto id = project.file_table.intern(Spelling::absolute(file));
        // The verdict is tied to the header's contents — a file edited
        // while the server was down must re-earn its trial.
        auto disk = project.file_table.current(id);
        if(!disk || disk->hash != entry.content_hash)
            continue;
        header_verdicts[id] = {.mode = HeaderMode::NeedsContext,
                               .content_hash = entry.content_hash};
    }
}

bool CommandResolver::fill_header_context_args(Fid path_id,
                                               std::string& directory,
                                               std::vector<std::string>& arguments,
                                               const CommandRequest& request,
                                               Resolution& resolution) {
    // Self-containment routing: an Unknown or SelfContained header borrows
    // the host command without a prefix; NeedsContext synthesizes one.
    // run_compile() flips Unknown to NeedsContext when the trial compile's
    // diagnostics indicate missing includer state. An explicitly chosen
    // occurrence — even #0 — only has meaning under includer-context
    // semantics, so it forces synthesis regardless of the verdict.
    auto path = project.file_table.resolve(path_id);
    const Selection* choice = request.selection;
    bool has_host_choice = choice && choice->host_path_id.valid();
    bool synthesize = header_mode(path_id) == HeaderMode::NeedsContext ||
                      (has_host_choice && choice->occurrence.has_value());

    // Use cached context if it is still valid; otherwise resolve. The cache
    // is dropped when an active context override points to a different host
    // or include occurrence, when the routing mode changed, or when any
    // chain file changed on disk (the synthesized context embeds their
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
            bool mode_mismatch = (context.synthesized != nullptr) != synthesize;
            auto wave = project.file_table.wave();
            if(override_mismatch || mode_mismatch ||
               deps_changed(project.file_table, context.deps)) {
                cache->erase(cached);
            } else {
                ctx_ptr = &context;
            }
        }
    }

    std::optional<HeaderContext> local_ctx;
    if(!ctx_ptr) {
        auto resolved = resolve_header_context(path_id, choice, synthesize);
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
    CanonicalRef edit_paths[] = {host_path, path};
    auto base = pick_pinned_config(project,
                                   path_id,
                                   commands,
                                   edit_paths,
                                   host_path,
                                   ctx_ptr->host_command_hash,
                                   ctx_ptr->host_base_hash)
                    .config;

    // The header compiles as the host's language, with the header injected
    // as the input; the synthesized prefix lands after the host's own
    // user-content flags (its -include runs first).
    auto ref =
        project.build.resolve(path_id, base, CommandSource::IncludeGraph, edit_paths, host_path);
    RenderOptions opts;
    if(ctx_ptr->synthesized) {
        opts.preamble = ctx_ptr->synthesized->prefix.c_str();
    }
    directory = project.cdb.config(ref.config).directory;
    arguments = to_strings(project.cdb.render(ref, opts));
    resolution.host = ctx_ptr->host_path_id;
    resolution.ref = ref;
    resolution.synthesized = ctx_ptr->synthesized;

    LOG_INFO("resolve_command: header context for {} (host={}, synthesized={})",
             path,
             host_path,
             ctx_ptr->synthesized != nullptr);
    return true;
}

Resolution CommandResolver::resolve_command(llvm::StringRef spelled,
                                            std::string& directory,
                                            std::vector<std::string>& arguments,
                                            const CommandRequest& request) {
    auto path_id = project.file_table.intern(Spelling::absolute(spelled));
    auto path = project.file_table.resolve(path_id);
    llvm::SmallVector<llvm::StringRef, 4> tried;
    Resolution resolution;

    // Render `base` with the rule edits of `paths` applied: an entry, a
    // default command, a borrowed or the builtin one alike.
    auto fill = [&](ConfigID base,
                    CommandSource source,
                    llvm::ArrayRef<CanonicalRef> paths,
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
        if(fill_header_context_args(path_id, directory, arguments, request, resolution)) {
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
        if(fill_header_context_args(path_id, directory, arguments, request, resolution)) {
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
        CanonicalRef edit_paths[] = {path, lender_path};
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

std::optional<HeaderContext> CommandResolver::resolve_header_context(Fid header_path_id,
                                                                     const Selection* choice,
                                                                     bool synthesize) {
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
        return HeaderContext{.host_path_id = host_path_id,
                             .occurrence = occurrence.value_or(0),
                             .host_command_hash = std::move(host_command_hash),
                             .host_base_hash = std::move(host_base_hash),
                             .chain = llvm::SmallVector<Fid>(chain.begin(), chain.end() - 1)};
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
    CanonicalRef edit_paths[] = {host_path, target_path};
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
        // Chain files are named by the build's spelling of each, so a
        // resolution matches the next one exactly when it is that file.
        auto found = project.file_table.intern(Spelling::absolute(result->path));
        return project.file_table.spelling(found).str();
    };

    // Read the chain files (all but the target) from disk. The synthesized
    // context deliberately reflects disk state, never open-document buffers:
    // open files must not be depended upon by other files. The versions
    // name the bytes just read — the bytes the synthesized context embeds.
    std::vector<std::string> chain_contents;
    std::vector<Spelling> chain_paths;
    llvm::SmallVector<ChainEntry> chain_entries;
    DepsSnapshot deps;
    chain_contents.reserve(chain.size() - 1);
    chain_paths.reserve(chain.size() - 1);
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
        chain_paths.push_back(project.file_table.spelling(chain[i]));
        chain_entries.push_back({chain_paths.back(), chain_contents.back()});
        project.file_table.observe(chain[i], observed->obs);
        deps.push_back(
            {.path_id = chain[i],
             .version = project.file_table.intern_version(chain[i], observed->obs.hash)});
    }

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the context must point at a copy. The snapshot
    // mirrors the header's disk state; re-synthesize when it changes so
    // other-occurrence expansions stay current.
    std::optional<llvm::StringRef> target_content;
    auto target_observed = read_file_observed(target_path.data());
    if(target_observed) {
        target_content = target_observed->content->getBuffer();
        project.file_table.observe(chain.back(), target_observed->obs);
        deps.push_back({.path_id = chain.back(),
                        .version = project.file_table.intern_version(chain.back(),
                                                                     target_observed->obs.hash)});
    }

    auto target_spelling = project.file_table.spelling(chain.back());
    auto synthesized =
        synthesize_context(chain_entries, target_spelling, resolver, occurrence, target_content);
    if(!synthesized) {
        LOG_WARN("resolve_header_context: cannot match include chain for {} (host={})",
                 target_path,
                 host_path);
        return std::nullopt;
    }

    return HeaderContext{.host_path_id = host_path_id,
                         .synthesized =
                             std::make_shared<const SynthesizedContext>(std::move(*synthesized)),
                         .occurrence = occurrence.value_or(0),
                         .host_command_hash = std::move(host_command_hash),
                         .host_base_hash = std::move(host_base_hash),
                         .chain = llvm::SmallVector<Fid>(chain.begin(), chain.end() - 1),
                         .deps = std::move(deps)};
}

}  // namespace clice
