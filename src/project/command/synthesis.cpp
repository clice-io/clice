module clice;

import :command.search_config;
import :project.command;
import :project.project;
import :support.logging;
import :syntax.include_resolver;
import :vfs.file_system;
import :vfs.path;

namespace clice {

Memo<Fid, HeaderContext>::Entry Project::synthesize(Fid header,
                                                    const Host& host,
                                                    const CommandRef& command) {
    auto& files = file_table;
    auto& contexts = hosting.contexts;
    auto command_hash = commands.command_hash(command.command);
    if(auto* entry = contexts.lookup(files, header)) {
        auto& known = entry->result;
        if(known.host == host.file && known.command == command_hash && known.chain == host.chain &&
           known.lines == host.lines && known.macro_braces == host.macro_braces) {
            return *entry;
        }
    }
    HeaderContext context{
        .host = host.file,
        .command = command_hash,
        .chain = host.chain,
        .lines = host.lines,
        .macro_braces = host.macro_braces,
    };
    auto& chain = host.chain;

    // Include directives along the chain are resolved with the host's real
    // search configuration, so same-named headers in different directories
    // cannot be confused.
    auto search_config = toolchain.search_config(command);
    vfs::Scope scope(files.dirs);
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
        auto found = files.intern(Spelling::absolute(result->path));
        result->path = files.spelling(found).str();
        return result;
    };

    // The versions name the bytes just read — the bytes the synthesized
    // context embeds.
    std::vector<std::string> contents;
    std::vector<Spelling> spellings;
    llvm::SmallVector<ChainEntry> entries;
    Inputs inputs;
    contents.reserve(chain.size() - 1);
    spellings.reserve(chain.size() - 1);
    auto commit = [&] {
        return contexts.commit(header, std::move(context), std::move(inputs));
    };
    for(std::size_t i = 0; i + 1 < chain.size(); i += 1) {
        auto path = files.resolve(chain[i]);
        auto observed = vfs::read_observed(path);
        if(!observed) {
            LOG_WARN("{}, for the context of {}", observed.error(), files.resolve(header));
            if(auto missing = current_input(files, chain[i])) {
                inputs.push_back(*missing);
            }
            return commit();
        }
        contents.push_back(std::move(observed->text));
        spellings.push_back(files.spelling(chain[i]));
        entries.push_back(
            {.path = spellings.back(),
             .content = contents.back(),
             .line = host.lines[i],
             .macro_braces = host.macro_braces[i]});
        files.disk.observe(chain[i], observed->obs);
        inputs.push_back(
            {.file = chain[i], .version = files.intern_version(chain[i], observed->obs.hash)});
    }

    // Snapshot the header itself for other occurrences along the chain:
    // its real path is remapped to the open buffer at compile time, so
    // includes of it inside the context must point at a copy. The snapshot
    // mirrors the header's disk state; a context embedding it is
    // synthesized again when it changes so other-occurrence expansions
    // stay current.
    std::optional<llvm::StringRef> target_content;
    auto target_observed = vfs::read_observed(files.resolve(header));
    if(target_observed) {
        target_content = target_observed->text;
        files.disk.observe(header, target_observed->obs);
    }
    auto synthesized =
        synthesize_context(entries, files.spelling(header), resolver, target_content);
    if(!synthesized) {
        LOG_WARN("Cannot match the include chain of {} under {}",
                 files.resolve(header),
                 files.resolve(host.file));
        return commit();
    }
    if(synthesized->snapshot) {
        inputs.push_back(
            {.file = header, .version = files.intern_version(header, target_observed->obs.hash)});
    }
    context.synthesized = std::make_shared<const SynthesizedContext>(std::move(*synthesized));
    return commit();
}

}  // namespace clice
