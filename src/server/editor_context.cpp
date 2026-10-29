module clice;

import :index.project_index;
import :project.command_resolver;
import :project.hosting;
import :server.editor_context;
import :support.json;
import :support.logging;

namespace clice {

namespace {

/// The context choices: user context choices — never invalidated by content,
/// only by the user or a vanished CDB anchor. Paths persist as spellings
/// and are re-interned at load. The structs mirror the on-disk JSON layout
/// field for field — changing them changes the format.

struct Entry {
    std::uint32_t file;  // index into the paths table
    std::uint32_t host;  // index into the paths table; ~0u = none
    std::uint32_t occurrence;
    std::string command_hash;
    std::string base_hash;
};

struct Blob {
    std::vector<std::string> paths;
    std::vector<Entry> contexts;
};

}  // namespace

Fid EditorContext::host_of(Fid path_id) {
    if(auto it = selections.find(path_id); it != selections.end()) {
        if(it->second.host_path_id.valid()) {
            return it->second.host_path_id;
        }
    }
    return clice::host_of(project, path_id);
}

const clang::LangOptions& EditorContext::dialect(Fid path_id) {
    return clice::dialect(project, path_id, selection(path_id), host_of(path_id));
}

Resolution EditorContext::resolve_command(Fid path_id,
                                          std::string& directory,
                                          std::vector<std::string>& arguments) {
    auto resolution = commands.resolve_command(path_id,
                                               directory,
                                               arguments,
                                               {
                                                   .selection = selection(path_id),
                                                   .open = true,
                                               });
    if(resolution.source == CommandSource::Inferred ||
       resolution.source == CommandSource::Fallback) {
        guessed_commands.insert(path_id);
    } else {
        guessed_commands.erase(path_id);
    }
    return resolution;
}

void EditorContext::mark_dirty() {
    blob.bytes = serialize();
    blob.dirty = true;
    blob.save_seq += 1;
    if(project.request_flush) {
        project.request_flush();
    }
}

std::string EditorContext::serialize() const {
    Blob data;
    llvm::StringMap<std::uint32_t> index_map;
    auto intern = [&](Fid fid) -> std::uint32_t {
        auto path = project.index.portable(project.file_table.resolve(fid));
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };

    for(auto& [path_id, saved]: selections) {
        Entry entry;
        entry.file = intern(path_id);
        entry.host = saved.host_path_id.valid() ? intern(saved.host_path_id) : ~0u;
        entry.occurrence = saved.occurrence.value_or(~0u);
        entry.command_hash = saved.command_hash;
        entry.base_hash = saved.base_hash;
        data.contexts.push_back(std::move(entry));
    }

    auto json = json::to_string<PathJsonConfig>(data);
    if(!json) {
        LOG_WARN("Failed to serialize the context choices");
        return {};
    }
    return std::move(*json);
}

void EditorContext::load() {
    if(blob.bytes.empty()) {
        return;
    }
    Blob data;
    if(!json::from_string(blob.bytes, data)) {
        LOG_WARN("Failed to parse the context choices");
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
            saved.host_path_id = project.file_table.intern(project.index.local(host));
        }
        if(entry.occurrence != ~0u) {
            saved.occurrence = entry.occurrence;
        }
        saved.command_hash = entry.command_hash;
        saved.base_hash = entry.base_hash;
        selections[project.file_table.intern(project.index.local(file))] = std::move(saved);
    }
}

bool EditorContext::pin_alive(Fid entry_file,
                              llvm::ArrayRef<CanonicalRef> paths,
                              const Selection& saved) const {
    auto entry_path = project.file_table.resolve(entry_file);
    for(auto& entry: project.build.commands(entry_file)) {
        if(!saved.base_hash.empty() &&
           project.commands.command_hash_hex(entry.command) == saved.base_hash) {
            return true;
        }
        auto ref =
            project.build.resolve(entry_file, entry.command, entry.source, paths, entry_path);
        if(project.commands.command_hash_hex(ref.command) == saved.command_hash) {
            return true;
        }
    }
    return false;
}

bool EditorContext::holds_choice(Fid path_id) {
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
        // No tree telling how often the host's compile enters the header
        // leaves the choice standing.
        auto occurrences = count_occurrences(project, host, path_id);
        if(occurrences == 0) {
            return false;
        }
        // A pinned occurrence can vanish while other inclusions of the
        // header survive.
        if(occurrences && saved->occurrence.has_value() && *saved->occurrence >= *occurrences) {
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
        // The drop must reach the context choices, or the stale choice
        // resurrects from disk at the next start.
        mark_dirty();
    }
}

}  // namespace clice
