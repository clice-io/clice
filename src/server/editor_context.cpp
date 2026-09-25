#include "server/editor_context.h"

#include <string>
#include <utility>
#include <vector>

#include "support/logging.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"

namespace clice {

namespace {

/// The contexts blob: user context choices and synthesized-artifact hosts
/// — never invalidated by content, only by the user or a vanished CDB
/// anchor. Paths persist as spellings and are re-interned at load. The
/// structs mirror the on-disk JSON layout field for field — changing them
/// changes the format.

struct CacheContextEntry {
    std::uint32_t file;  // index into the paths table
    std::uint32_t host;  // index into the paths table; ~0u = none
    std::uint32_t occurrence;
    std::string command_hash;
    std::string base_hash;
};

struct CacheArtifactEntry {
    std::uint32_t file;  // index into the paths table
    std::uint32_t host;  // index into the paths table
};

struct ContextsData {
    std::vector<std::string> paths;
    std::vector<CacheContextEntry> contexts;
    std::vector<CacheArtifactEntry> artifacts;
};

}  // namespace

Resolution EditorContext::resolve_command(llvm::StringRef path,
                                          std::string& directory,
                                          std::vector<std::string>& arguments) {
    auto path_id = project.file_table.intern(path);
    auto resolution = commands.resolve_command(path,
                                               directory,
                                               arguments,
                                               {
                                                   .selection = selection(path_id),
                                                   .header_contexts = &header_contexts,
                                                   .synthesized_hosts = &synthesized_hosts,
                                               });
    if(resolution.source == CommandSource::Inferred ||
       resolution.source == CommandSource::Fallback) {
        guessed_commands.insert(path_id);
    } else {
        guessed_commands.erase(path_id);
    }
    for(auto& file: resolution.synthesized) {
        record_synthesized_host(file.path, file.host);
    }
    return resolution;
}

void EditorContext::invalidate_header_deps(Fid path_id) {
    auto* context = header_context(path_id);
    if(!context) {
        return;
    }
    if(context->deps.empty()) {
        drop_header_context(path_id);
    } else {
        force_revalidate_deps(project.file_table, context->deps);
    }
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
    auto intern_path = [&](llvm::StringRef path) -> std::uint32_t {
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path.str());
        }
        return it->second;
    };
    auto intern = [&](Fid fid) -> std::uint32_t {
        return intern_path(project.file_table.resolve(fid));
    };

    for(auto& entry: synthesized_hosts) {
        data.artifacts.push_back({intern_path(entry.getKey()), intern(entry.second)});
    }
    for(auto& [path_id, saved]: selections) {
        CacheContextEntry entry;
        entry.file = intern(path_id);
        entry.host = saved.host_path_id.valid() ? intern(saved.host_path_id) : ~0u;
        entry.occurrence = saved.occurrence.value_or(~0u);
        entry.command_hash = saved.command_hash;
        entry.base_hash = saved.base_hash;
        data.contexts.push_back(std::move(entry));
    }

    auto json = kota::codec::json::to_string(data);
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
            saved.host_path_id = project.file_table.intern(host);
        }
        if(entry.occurrence != ~0u) {
            saved.occurrence = entry.occurrence;
        }
        saved.command_hash = entry.command_hash;
        saved.base_hash = entry.base_hash;
        selections[project.file_table.intern(file)] = std::move(saved);
    }

    bool pruned = false;
    for(auto& entry: data.artifacts) {
        auto file = resolve(entry.file);
        auto host = resolve(entry.host);
        if(file.empty() || host.empty())
            continue;
        // A file the store evicted since (or a wiped cache) has nothing
        // left to open under the host's command; the record leaves the
        // blob with the next save.
        if(!llvm::sys::fs::exists(file)) {
            pruned = true;
            continue;
        }
        synthesized_hosts[file] = project.file_table.intern(host);
    }
    if(pruned) {
        mark_dirty();
    }
}

void EditorContext::drop_evicted_artifacts() {
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
        mark_dirty();
    }
}

void EditorContext::record_synthesized_host(llvm::StringRef path, Fid host_path_id) {
    auto [it, inserted] = synthesized_hosts.try_emplace(path, host_path_id);
    if(!inserted && it->second == host_path_id) {
        return;
    }
    it->second = host_path_id;
    mark_dirty();
}

void EditorContext::append_suffix_include(Fid path_id, std::string& text) const {
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

bool EditorContext::pin_alive(Fid entry_file,
                              llvm::ArrayRef<llvm::StringRef> paths,
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
        if(project.build.commands(host).empty() ||
           project.dep_graph.find_include_chain(host, path_id).empty()) {
            return false;
        }
        // A pinned occurrence can vanish while other inclusions of the
        // header survive (the chain stays non-empty).
        if(saved->occurrence.has_value()) {
            auto count = project.count_occurrences(host, path_id);
            if(count > 0 && *saved->occurrence >= count) {
                return false;
            }
        }
        llvm::StringRef edit_paths[] = {project.file_table.resolve(host), path};
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
