module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument_parser;
import :project.command_resolver;
import :project.hosting;
import :server.editor_context;
import :support.json;
import :support.logging;
import :syntax.lexer;

namespace clice {

namespace {

/// The contexts blob: user context choices — never invalidated by content,
/// only by the user or a vanished CDB anchor. Paths persist as spellings
/// and are re-interned at load. The structs mirror the on-disk JSON layout
/// field for field — changing them changes the format.

struct CacheContextEntry {
    std::uint32_t file;  // index into the paths table
    std::uint32_t host;  // index into the paths table; ~0u = none
    std::uint32_t occurrence;
    std::string command_hash;
    std::string base_hash;
};

struct ContextsData {
    std::vector<std::string> paths;
    std::vector<CacheContextEntry> contexts;
};

/// The command a file compiles with — among a multi-configuration
/// file's or host's entries the pinned one, else the first, or the default
/// command covering it — rules applied, like the resolve path's effective
/// command: whether it compiles the file as C, whether an -x forces that
/// (the driver override beats every suffix heuristic), and the last -std
/// value.
struct CommandLang {
    CommandSource source;
    bool is_c = false;
    bool language_forced = false;
    std::string standard;
};

std::optional<CommandLang> command_lang(Project& project,
                                        Fid file,
                                        llvm::ArrayRef<CanonicalRef> paths,
                                        const Selection* pin) {
    auto path = project.file_table.resolve(file);
    auto commands = project.build.commands(file);
    if(commands.empty()) {
        return std::nullopt;
    }
    auto command = pick_pinned_config(project,
                                      file,
                                      commands,
                                      paths,
                                      path,
                                      pin ? llvm::StringRef(pin->command_hash) : "",
                                      pin ? llvm::StringRef(pin->base_hash) : "");
    auto ref = project.build.resolve(file, command.config, command.source, paths, path);

    llvm::StringRef language = ref.input.value;
    CommandLang result{
        .source = command.source,
        .is_c = language == "c" || language == "c-header",
        .language_forced = !project.cdb.forced_language(ref.config).empty(),
    };
    for(auto& arg: project.cdb.config(ref.config).args) {
        if(arg.opt_id == option::OPT_std_EQ && arg.values.size() == 1) {
            result.standard = arg.values[0];
        }
    }
    return result;
}

}  // namespace

Fid EditorContext::host_of(Fid path_id) const {
    if(auto it = selections.find(path_id); it != selections.end()) {
        if(it->second.host_path_id.valid()) {
            return it->second.host_path_id;
        }
    }
    if(const auto* context = header_context(path_id)) {
        return context->host_path_id;
    }
    // A header compiled under its own entry resolves no context; its
    // includers still name the source its definitions belong in.
    auto hosts = ranked_hosts(project, path_id);
    return hosts.empty() ? Fid{} : hosts.front();
}

const clang::LangOptions& EditorContext::dialect(Fid path_id) {
    auto path = project.file_table.resolve(path_id);
    const auto* pin = selection(path_id);
    bool host_pin = pin && pin->host_path_id.valid();
    auto own = command_lang(project, path_id, path, host_pin ? nullptr : pin);
    // A file's entry is its command; a default command yields to the host
    // a header borrows from, in resolve_command's order.
    if(own && own->language_forced && own->source == CommandSource::CDBExact) {
        return dialect_of("", own->is_c, own->standard);
    }

    // A header's active context (the user's persisted choice, else the
    // resolved host) names the view being read; its command beats the
    // contributor union the way it does for the AST after an escalation.
    if(Fid host = host_of(path_id); host.valid()) {
        CanonicalRef edit_paths[] = {project.file_table.resolve(host), path};
        auto host_lang = command_lang(project, host, edit_paths, host_pin ? pin : nullptr);
        if(host_lang) {
            return dialect_of("", host_lang->is_c, host_lang->standard);
        }
    }

    if(own && own->language_forced) {
        return dialect_of("", own->is_c, own->standard);
    }

    auto& contributions = project.project_index.contributions;
    auto it = contributions.find(path_id);
    bool c_only = it != contributions.end() && !it->second.empty() &&
                  llvm::all_of(llvm::make_first_range(it->second), [&](Fid tu) {
                      return llvm::StringRef(project.file_table.resolve(tu)).ends_with(".c");
                  });
    return dialect_of(path, c_only, own ? llvm::StringRef(own->standard) : llvm::StringRef());
}

Resolution EditorContext::resolve_command(Fid path_id,
                                          std::string& directory,
                                          std::vector<std::string>& arguments) {
    auto resolution = commands.resolve_command(path_id,
                                               directory,
                                               arguments,
                                               {
                                                   .selection = selection(path_id),
                                                   .header_contexts = &header_contexts,
                                               });
    if(resolution.source == CommandSource::Inferred ||
       resolution.source == CommandSource::Fallback) {
        guessed_commands.insert(path_id);
    } else {
        guessed_commands.erase(path_id);
    }
    return resolution;
}

llvm::SmallVector<Fid> EditorContext::chain_dependents(Fid path_id) const {
    llvm::SmallVector<Fid> result;
    for(auto& [header_id, context]: header_contexts) {
        if(llvm::is_contained(context.chain, path_id)) {
            result.push_back(header_id);
        }
    }
    return result;
}

void EditorContext::mark_dirty() {
    blob.bytes = serialize();
    blob.dirty = true;
    blob.ticket += 1;
    if(project.request_flush) {
        project.request_flush();
    }
}

std::string EditorContext::serialize() const {
    ContextsData data;
    llvm::StringMap<std::uint32_t> index_map;
    auto intern = [&](Fid fid) -> std::uint32_t {
        auto path = project.project_index.portable(project.file_table.resolve(fid));
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };

    for(auto& [path_id, saved]: selections) {
        CacheContextEntry entry;
        entry.file = intern(path_id);
        entry.host = saved.host_path_id.valid() ? intern(saved.host_path_id) : ~0u;
        entry.occurrence = saved.occurrence.value_or(~0u);
        entry.command_hash = saved.command_hash;
        entry.base_hash = saved.base_hash;
        data.contexts.push_back(std::move(entry));
    }

    auto json = kota::codec::json::to_string<PathJsonConfig>(data);
    if(!json) {
        LOG_WARN("Failed to serialize the contexts blob");
        return {};
    }
    return std::move(*json);
}

void EditorContext::load() {
    if(blob.bytes.empty()) {
        return;
    }
    ContextsData data;
    if(!kota::codec::json::from_string(blob.bytes, data)) {
        LOG_WARN("Failed to parse the contexts blob");
        return;
    }
    auto resolve = [&](std::uint32_t idx) -> llvm::StringRef {
        return idx < data.paths.size() ? llvm::StringRef(data.paths[idx]) : "";
    };

    for(auto& entry: data.contexts) {
        auto file = resolve(entry.file);
        if(file.empty())
            continue;
        Selection saved;
        if(entry.host != ~0u) {
            auto host = resolve(entry.host);
            if(host.empty())
                continue;
            saved.host_path_id = project.file_table.intern(project.project_index.local(host));
        }
        if(entry.occurrence != ~0u) {
            saved.occurrence = entry.occurrence;
        }
        saved.command_hash = entry.command_hash;
        saved.base_hash = entry.base_hash;
        selections[project.file_table.intern(project.project_index.local(file))] = std::move(saved);
    }
}

bool EditorContext::pin_alive(Fid entry_file,
                              llvm::ArrayRef<CanonicalRef> paths,
                              const Selection& saved) const {
    auto entry_path = project.file_table.resolve(entry_file);
    for(auto& entry: project.build.commands(entry_file)) {
        if(!saved.base_hash.empty() &&
           project.cdb.entry_hash_hex(entry.config) == saved.base_hash) {
            return true;
        }
        auto ref = project.build.resolve(entry_file, entry.config, entry.source, paths, entry_path);
        if(project.cdb.entry_hash_hex(ref.config) == saved.command_hash) {
            return true;
        }
    }
    return false;
}

bool EditorContext::holds_choice(Fid path_id) const {
    auto* saved = selection(path_id);
    if(!saved) {
        return false;
    }
    auto path = project.file_table.resolve(path_id);
    if(saved->host_path_id.valid()) {
        auto host = saved->host_path_id;
        if(project.build.commands(host).empty()) {
            return false;
        }
        auto occurrences = count_occurrences(project, host, path_id);
        if(occurrences == 0) {
            return false;
        }
        // A pinned occurrence can vanish while other inclusions of the
        // header survive.
        if(saved->occurrence.has_value() && *saved->occurrence >= occurrences) {
            return false;
        }
        CanonicalRef edit_paths[] = {project.file_table.resolve(host), path};
        return saved->command_hash.empty() || pin_alive(host, edit_paths, *saved);
    }
    return !saved->command_hash.empty() && !project.build.commands(path_id).empty() &&
           pin_alive(path_id, path, *saved);
}

void EditorContext::validate_saved_context(Fid path_id) {
    // A context choice persisted from an earlier session stays authoritative
    // only if it still holds: the CDB or include graph may have changed
    // while the server was down, and a stale choice suppresses automatic
    // host resolution and strands the file on the fallback command.
    if(selection(path_id) && !holds_choice(path_id)) {
        LOG_INFO("didOpen: dropping stale saved context for {}",
                 project.file_table.resolve(path_id));
        selections.erase(path_id);
        // The drop must reach the contexts blob, or the stale choice
        // resurrects from disk at the next start.
        mark_dirty();
    }
}

}  // namespace clice
