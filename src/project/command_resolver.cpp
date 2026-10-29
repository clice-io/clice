module clice;

import :command.argument;
import :command.file_kind;
import :command.search_config;
import :index.project_index;
import :project.command_resolver;
import :project.hosting;
import :support.logging;
import :syntax.header_context;
import :syntax.include_resolver;
import :syntax.lexer;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice {

namespace {

/// Per-file command selection decision log: which tiers were tried, which one
/// was hit, and a hash of the final command for correlating later failures.
void log_command_decision(llvm::StringRef path,
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

/// How many of `ref`'s `-include`s come before the one naming `forced`:
/// what a header context entered through that forced file keeps
/// (HeaderContext::forced). All of them for none.
std::size_t includes_before(Project& project, const CommandRef& ref, Fid forced) {
    if(!forced.valid()) {
        return std::numeric_limits<std::size_t>::max();
    }
    auto search = project.toolchain.search_config(ref);
    vfs::Scope scope(project.file_table.dirs);
    auto resolved = resolve_search_config(search, scope);
    llvm::StringRef directory = project.commands.get(ref.command).directory;
    for(std::size_t i = 0; i < search.forced_includes.size(); i += 1) {
        auto header = resolve_forced_include(search.forced_includes[i], directory, resolved, scope);
        if(header && project.file_table.find(Spelling::absolute(header->path)) == forced) {
            return i;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

/// The language of the command a file compiles with — among a
/// multi-configuration file's or host's entries the pinned one, else the
/// first, or the default command covering it — rules applied, like the
/// resolve path's effective command: whether it compiles the file as C,
/// whether an -x forces that (the driver override beats every suffix
/// heuristic), and the last -std value.
struct Language {
    CommandSource source;
    bool is_c = false;
    bool language_forced = false;
    std::string standard;
};

std::optional<Language> language_of(Project& project,
                                    Fid file,
                                    llvm::ArrayRef<CanonicalRef> paths,
                                    const Selection* pin) {
    auto path = project.file_table.resolve(file);
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto candidate = pick_pinned_command(project,
                                         file,
                                         commands,
                                         paths,
                                         path,
                                         pin ? llvm::StringRef(pin->command_hash) : "",
                                         pin ? llvm::StringRef(pin->base_hash) : "");
    auto ref = project.build.resolve(file, candidate.command, candidate.source, paths, path);

    llvm::StringRef language = ref.input.value;
    Language result{
        .source = candidate.source,
        .is_c = language == "c" || language == "c-header",
        .language_forced = !project.commands.forced_language(ref.command).empty(),
    };
    for(auto& arg: project.commands.get(ref.command).args) {
        if(arg.opt_id == option::OPT_std_EQ && arg.values.size() == 1) {
            result.standard = arg.values[0];
        }
    }
    return result;
}

}  // namespace

Candidate pick_pinned_command(Project& project,
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
            if(project.commands.command_hash_hex(entry.command) == pinned_base) {
                return entry;
            }
        }
    }
    if(!pinned_hash.empty()) {
        for(auto& entry: candidates) {
            auto ref =
                project.build.resolve(file, entry.command, entry.source, paths, language_path);
            if(project.commands.command_hash_hex(ref.command) == pinned_hash) {
                return entry;
            }
        }
    }
    return candidates.front();
}

CommandRef lent_command(Project& project,
                        Fid header,
                        Fid host,
                        CommandID command,
                        llvm::ArrayRef<std::string> extra_prepend,
                        llvm::ArrayRef<std::string> extra_append) {
    // The header inherits the host's world: the rules matching the host
    // and the rules matching the header both edit the borrowed command,
    // each once, in declaration order.
    auto host_path = project.file_table.resolve(host);
    CanonicalRef edit_paths[] = {host_path, project.file_table.resolve(header)};
    return project.build.resolve(header,
                                 command,
                                 CommandSource::IncludeGraph,
                                 edit_paths,
                                 host_path,
                                 extra_prepend,
                                 extra_append);
}

std::optional<CommandRef> host_command(Project& project,
                                       Fid header,
                                       Fid host,
                                       llvm::StringRef pinned_hash,
                                       llvm::StringRef pinned_base,
                                       llvm::ArrayRef<std::string> extra_prepend,
                                       llvm::ArrayRef<std::string> extra_append) {
    auto commands = host_commands(project, header, host);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto host_path = project.file_table.resolve(host);
    CanonicalRef edit_paths[] = {host_path, project.file_table.resolve(header)};
    auto picked = pick_pinned_command(project,
                                      header,
                                      commands,
                                      edit_paths,
                                      host_path,
                                      pinned_hash,
                                      pinned_base);
    return lent_command(project, header, host, picked.command, extra_prepend, extra_append);
}

const clang::LangOptions& dialect(Project& project, Fid file, const Selection* pin, Fid host) {
    auto path = project.file_table.resolve(file);
    bool host_pin = pin && pin->host_path_id.valid();
    auto own = language_of(project, file, path, host_pin ? nullptr : pin);
    // A file's entry is its command; a default command yields to the host
    // a header borrows from, in resolve_command's order.
    if(own && own->language_forced && own->source == CommandSource::CDBExact) {
        return dialect_of("", own->is_c, own->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    if(host.valid()) {
        CanonicalRef edit_paths[] = {project.file_table.resolve(host), path};
        auto host_lang = language_of(project, host, edit_paths, host_pin ? pin : nullptr);
        if(host_lang) {
            return dialect_of("", host_lang->is_c, host_lang->standard);
        }
    }

    if(own && own->language_forced) {
        return dialect_of("", own->is_c, own->standard);
    }

    bool contributed = false;
    bool c_only = true;
    project.index.for_each_contribution(file, [&](Fid tu, std::uint64_t) {
        contributed = true;
        c_only = c_only && llvm::StringRef(project.file_table.resolve(tu)).ends_with(".c");
    });
    c_only = contributed && c_only;
    return dialect_of(path, c_only, own ? llvm::StringRef(own->standard) : llvm::StringRef());
}

HeaderMode CommandResolver::header_mode(Fid path_id) const {
    if(is_context_header_path(project.file_table.resolve(path_id))) {
        return HeaderMode::NeedsContext;
    }
    if(auto it = header_results.find(path_id); it != header_results.end()) {
        return it->second.mode;
    }
    return HeaderMode::Unknown;
}

void CommandResolver::forget_self_contained(Fid path_id) {
    if(header_mode(path_id) == HeaderMode::SelfContained) {
        reset_header_mode(path_id);
    }
}

std::uint64_t CommandResolver::persisted_mode_hash(Fid path_id) const {
    auto it = header_results.find(path_id);
    if(it == header_results.end() || it->second.mode != HeaderMode::NeedsContext) {
        return 0;
    }
    return it->second.content_hash;
}

void CommandResolver::record_header_mode(Fid path_id, HeaderMode mode, std::uint64_t content_hash) {
    auto persisted = persisted_mode_hash(path_id);
    if((header_mode(path_id) == HeaderMode::SelfContained) != (mode == HeaderMode::SelfContained)) {
        project.context_epoch += 1;
    }
    header_results[path_id] = {
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
    if(header_mode(path_id) == HeaderMode::SelfContained) {
        project.context_epoch += 1;
    }
    header_results.erase(path_id);
}

void CommandResolver::dump_context_headers(std::vector<ContextHeaderEntry>& headers,
                                           llvm::function_ref<std::uint32_t(Fid)> intern_id) const {
    for(auto& [path_id, result]: header_results) {
        // A result scored with no disk observation (hash 0) cannot be
        // validated on load, so it stays in memory: persisted, it would
        // skip the self-containment trial for whatever bytes the next
        // session finds on disk.
        if(result.mode != HeaderMode::NeedsContext || result.content_hash == 0)
            continue;
        headers.push_back({intern_id(path_id), result.content_hash});
    }
}

void CommandResolver::load_context_headers(
    llvm::ArrayRef<ContextHeaderEntry> headers,
    llvm::function_ref<std::optional<Fid>(std::uint32_t)> file_of) {
    for(auto& entry: headers) {
        // The writer never emits unbound (hash 0) results; an entry
        // carrying one is corrupt and must not bypass the content gate.
        if(entry.content_hash == 0)
            continue;
        auto id = file_of(entry.file);
        if(!id)
            continue;
        // The result is tied to the header's contents — a file edited
        // while the server was down must re-earn its trial.
        auto disk = project.file_table.disk.current(*id);
        if(!disk || disk->hash != entry.content_hash)
            continue;
        header_results[*id] = {.mode = HeaderMode::NeedsContext,
                               .content_hash = entry.content_hash};
    }
}

std::optional<CommandRef> CommandResolver::resolve_through_host(Fid path_id,
                                                                const CommandRequest& request,
                                                                Resolution& resolution) {
    // Self-containment routing: an Unknown or SelfContained header borrows
    // the host command without a prefix; NeedsContext synthesizes one.
    // ASTFamily::run() flips Unknown to NeedsContext when the trial compile's
    // diagnostics indicate missing includer state. A host the user chose
    // is chosen for its preprocessor state, so it always synthesizes.
    auto path = project.file_table.resolve(path_id);
    const Selection* choice = request.selection;
    bool has_host_choice = choice && choice->host_path_id.valid();
    bool synthesize = has_host_choice || header_mode(path_id) == HeaderMode::NeedsContext;

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
            bool mode_mismatch =
                (context.synthesized != nullptr) != (synthesize && !context.chain.empty());
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
        auto resolved = resolve_header_context(path_id, request, synthesize);
        if(!resolved) {
            resolution.unmatched_host = resolved.error();
            resolution.tree_wanted = resolved.error();
            return std::nullopt;
        }
        if(cache) {
            ctx_ptr = &((*cache)[path_id] = std::move(*resolved));
        } else {
            local_ctx = std::move(*resolved);
            ctx_ptr = &*local_ctx;
        }
    }
    if(ctx_ptr->synthesized && ctx_ptr->lexical) {
        resolution.tree_wanted = ctx_ptr->host_path_id;
    }

    auto host_path = project.file_table.resolve(ctx_ptr->host_path_id);
    auto ref = host_command(project,
                            path_id,
                            ctx_ptr->host_path_id,
                            ctx_ptr->host_command_hash,
                            ctx_ptr->host_base_hash,
                            request.extra_prepend,
                            request.extra_append);
    if(!ref) {
        LOG_WARN("resolve_through_host: host {} has no compile command", host_path);
        return std::nullopt;
    }
    resolution.host = ctx_ptr->host_path_id;
    resolution.forced = ctx_ptr->forced;
    resolution.synthesized = ctx_ptr->synthesized;

    LOG_INFO("resolve_command: header context for {} (host={}, synthesized={})",
             path,
             host_path,
             ctx_ptr->synthesized != nullptr);
    return ref;
}

Resolution CommandResolver::resolve_command(Fid path_id,
                                            std::string& directory,
                                            std::vector<std::string>& arguments,
                                            const CommandRequest& request) {
    auto path = project.file_table.resolve(path_id);
    llvm::SmallVector<llvm::StringRef, 4> tried;
    Resolution resolution;

    auto finish = [&](const CommandRef& ref) {
        // A header borrowing its host's command compiles as the host's
        // language, with the header injected as the input; the synthesized
        // prefix lands after the host's own user-content flags (its
        // -includes run first, but for the one the context enters the
        // header through and those after it).
        RenderOptions options;
        if(resolution.synthesized) {
            options.preamble = resolution.synthesized->prefix.c_str();
        }
        options.kept_includes = includes_before(project, ref, resolution.forced);
        directory = project.commands.get(ref.command).directory;
        auto rendered = project.toolchain.render(ref, project.cdb.spelling(ref.file), options);
        arguments.assign(rendered.argv.begin(), rendered.argv.end());
        // The builtin command names a compiler no build chose: clang's own
        // reading of it, when that compiler is missing, loses nothing.
        if(ref.source != CommandSource::Fallback) {
            resolution.query_failure = std::move(rendered.query_failure);
        }
        resolution.hidden = std::move(rendered.hidden);
        resolution.ref = ref;
        resolution.source = ref.source;
        log_command_decision(path, tried, ref.source, arguments);
        return std::move(resolution);
    };

    // `base` with the rule edits of `paths` and the run's extras applied:
    // an entry, a default command, a borrowed or the builtin one alike.
    auto finish_own = [&](CommandID base,
                          CommandSource source,
                          llvm::ArrayRef<CanonicalRef> paths,
                          llvm::StringRef language_path) {
        return finish(project.build.resolve(path_id,
                                            base,
                                            source,
                                            paths,
                                            language_path,
                                            request.extra_prepend,
                                            request.extra_append));
    };

    const Selection* choice = request.selection;
    bool has_host_choice = choice && choice->host_path_id.valid();

    // 1. If the file has an active header context via switchContext, use the
    //    host source's CDB entry with file path replaced and preamble injected.
    if(has_host_choice) {
        tried.push_back("switch_context");
        if(auto ref = resolve_through_host(path_id, request, resolution)) {
            return finish(*ref);
        }
    }

    // 2. The file's own command: a database entry, the default command of
    //    a source the build compiles as a unit, or the command a
    //    provisional member borrows. A header's default command is only
    //    the last resort below, after host inference.
    //    Multi-config projects honor the user's chosen entry, matched by
    //    command hash so the choice survives reordering.
    tried.push_back("cdb");
    auto commands = project.build.commands(path_id);
    if(project.build.unit(path_id)) {
        llvm::StringRef pinned_hash, pinned_base;
        if(choice && !has_host_choice) {
            pinned_hash = choice->command_hash;
            pinned_base = choice->base_hash;
        }
        auto picked =
            pick_pinned_command(project, path_id, commands, path, path, pinned_hash, pinned_base);
        return finish_own(picked.command, picked.source, path, path);
    }

    // 3. No CDB entry — try automatic header context resolution.
    if(!has_host_choice) {
        tried.push_back("include_graph");
        if(auto ref = resolve_through_host(path_id, request, resolution)) {
            return finish(*ref);
        }
    }

    // 4. A rule's default command for a file the build does not compile as
    //    a unit (a header under a default-command rule).
    if(!commands.empty()) {
        tried.push_back("default");
        return finish_own(commands.front().command, CommandSource::Default, path, path);
    }

    // 5. A nearby unit's command, edited for both files as a provisional
    //    member's is. A header compiles in that unit's language — a `.h`
    //    as C++, not as a header job; a source in its own, which lending
    //    only ever matches or crosses between C++ and its modules.
    tried.push_back("inferred");
    if(auto lender = command_lender(project, path_id)) {
        auto lender_path = project.file_table.resolve(lender->unit);
        LOG_INFO("resolve_command: {} borrows the command of {}", path, lender_path);
        resolution.lender = lender->unit;
        return finish_own(project.build.lend(lender->command, lender_path, path),
                          CommandSource::Inferred,
                          path,
                          is_header_path(path) ? lender_path : path);
    }

    // 6. The builtin fallback, so the file still compiles and produces
    //    diagnostics instead of failing silently.
    tried.push_back("fallback");
    return finish_own(project.build.builtin(path), CommandSource::Fallback, path, path);
}

std::optional<CommandRef> CommandResolver::pch_unit(Fid path_id,
                                                    const Resolution& resolution,
                                                    const CommandRequest& request) {
    llvm::SmallVector<CanonicalRef, 2> paths{project.file_table.resolve(path_id)};
    if(resolution.host.valid()) {
        paths.push_back(project.file_table.resolve(resolution.host));
    }
    return project.build.pch_unit(resolution.ref.command,
                                  paths,
                                  request.extra_prepend,
                                  request.extra_append);
}

std::expected<HeaderContext, Fid>
    CommandResolver::resolve_header_context(Fid header_path_id,
                                            const CommandRequest& request,
                                            bool synthesize) {
    const Selection* choice = request.selection;
    // A pinned host (and its chosen include occurrence) wins while it
    // still compiles and still includes the header; otherwise the build's
    // default host.
    std::optional<Host> host;
    std::optional<std::uint32_t> occurrence;
    bool has_host_choice = choice && choice->host_path_id.valid();
    if(has_host_choice) {
        auto preferred = choice->host_path_id;
        if(!project.build.commands(preferred).empty()) {
            if(auto found = enterings(project, preferred, header_path_id)) {
                auto n = choice->occurrence.value_or(0);
                if(n < found->size()) {
                    host = std::move((*found)[n]);
                    occurrence = n;
                }
            } else if(auto chain = lexical_chain(project, preferred, header_path_id);
                      !chain.empty()) {
                host = Host{.file = preferred, .chain = std::move(chain), .lexical = true};
                occurrence = choice->occurrence;
            }
        }
    }
    if(!host) {
        host = default_host(project, header_path_id);
        if(!host) {
            return std::unexpected(Fid{});
        }
    }
    auto host_path_id = host->file;
    auto& chain = host->chain;

    // Self-contained route: borrow the host's command, no prefix needed.
    // The chain is kept so a didSave along it still invalidates the session.
    std::string host_command_hash;
    std::string host_base_hash;
    if(has_host_choice) {
        host_command_hash = choice->command_hash;
        host_base_hash = choice->base_hash;
    }

    bool lexical = host->lexical;
    Fid forced = chain.front() != host_path_id ? chain.front() : Fid{};
    // A header the command forces in itself has nothing to cut: the
    // -includes ahead of it are its context.
    if(!synthesize || chain.size() == 1) {
        return HeaderContext{.host_path_id = host_path_id,
                             .occurrence = occurrence.value_or(0),
                             .host_command_hash = std::move(host_command_hash),
                             .host_base_hash = std::move(host_base_hash),
                             .chain = llvm::SmallVector<Fid>(chain.begin(), chain.end() - 1),
                             .forced = forced,
                             .lexical = lexical};
    }

    // Include directives along the chain are resolved with the host's real
    // search configuration, so same-named headers in different directories
    // cannot be confused.
    auto host_ref = host_command(project,
                                 chain.back(),
                                 host_path_id,
                                 host_command_hash,
                                 host_base_hash,
                                 request.extra_prepend,
                                 request.extra_append);
    if(!host_ref) {
        return std::unexpected(host_path_id);
    }
    auto host_path = project.file_table.resolve(host_path_id);
    auto target_path = project.file_table.resolve(chain.back());

    auto search_config = project.toolchain.search_config(*host_ref);
    vfs::Scope scope(project.file_table.dirs);
    auto resolved_config = resolve_search_config(search_config, scope);

    auto resolver =
        [&](const ScanResult::IncludeInfo& include,
            llvm::StringRef includer_dir,
            std::optional<unsigned> includer_found_dir) -> std::optional<ResolveResult> {
        auto result = resolve_include(include.path,
                                      include.is_angled,
                                      &scope.list(includer_dir),
                                      includer_dir,
                                      include.is_include_next,
                                      includer_found_dir,
                                      resolved_config,
                                      scope);
        if(!result) {
            return std::nullopt;
        }
        // Chain files are named by the build's spelling of each, so a
        // resolution matches the next one exactly when it is that file.
        auto found = project.file_table.intern(Spelling::absolute(result->path));
        result->path = project.file_table.spelling(found).str();
        return result;
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
        auto observed = vfs::read_observed(cur_path);
        if(!observed) {
            LOG_WARN("resolve_header_context: cannot read {}", cur_path);
            return std::unexpected(host_path_id);
        }
        chain_contents.push_back(std::move(observed->text));
        chain_paths.push_back(project.file_table.spelling(chain[i]));
        chain_entries.push_back({
            .path = chain_paths.back(),
            .content = chain_contents.back(),
            .line = host->lines.empty() ? 0 : host->lines[i],
        });
        project.file_table.disk.observe(chain[i], observed->obs);
        deps.push_back(
            {.path_id = chain[i],
             .version = project.file_table.intern_version(chain[i], observed->obs.hash)});
    }

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the context must point at a copy. The snapshot
    // mirrors the header's disk state; a context embedding it re-synthesizes
    // when it changes so other-occurrence expansions stay current.
    std::optional<llvm::StringRef> target_content;
    auto target_observed = vfs::read_observed(target_path);
    if(target_observed) {
        target_content = target_observed->text;
        project.file_table.disk.observe(chain.back(), target_observed->obs);
    }

    auto target_spelling = project.file_table.spelling(chain.back());
    auto synthesized = synthesize_context(chain_entries,
                                          target_spelling,
                                          resolver,
                                          host->lines.empty() ? occurrence : std::nullopt,
                                          target_content);
    if(!synthesized) {
        LOG_WARN("resolve_header_context: cannot match include chain for {} (host={})",
                 target_path,
                 host_path);
        return std::unexpected(host_path_id);
    }
    if(synthesized->snapshot) {
        deps.push_back({.path_id = chain.back(),
                        .version = project.file_table.intern_version(chain.back(),
                                                                     target_observed->obs.hash)});
    }

    return HeaderContext{.host_path_id = host_path_id,
                         .synthesized =
                             std::make_shared<const SynthesizedContext>(std::move(*synthesized)),
                         .occurrence = occurrence.value_or(0),
                         .host_command_hash = std::move(host_command_hash),
                         .host_base_hash = std::move(host_base_hash),
                         .chain = llvm::SmallVector<Fid>(chain.begin(), chain.end() - 1),
                         .forced = forced,
                         .deps = std::move(deps),
                         .lexical = lexical};
}

}  // namespace clice
