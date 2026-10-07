module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.serialization;
import :project.project;
import :support.logging;
import :vfs.file_system;
import :vfs.path;

namespace clice {

void Project::rescan_disk_file(Fid path_id) {
    rescan_dependency_graph(cdb, dep_graph, path_id);
    context_epoch += 1;
}

void Project::forget_file(Fid path_id) {
    dep_graph.update_module_decl(path_id, {});
    dep_graph.forget_scan(path_id);
    dep_graph.clear_includes(path_id);
    context_epoch += 1;
}

Project::ProviderChanges Project::rebuild_dependency_graph() {
    llvm::StringMap<Fid> selected;
    for(auto& entry: dep_graph.modules()) {
        if(!entry.getValue().empty()) {
            selected[entry.getKey()] = entry.getValue().front();
        }
    }

    // TODO: this scan runs synchronously on the event loop (same cost as
    // the startup scan); if it shows up on large projects, move it off the
    // dispatch path.
    dep_graph.reset();
    scan_dependency_graph(cdb, dep_graph, build.units(build.members()));
    dep_graph.build_reverse_map();
    context_epoch += 1;

    ProviderChanges changes;
    for(auto& entry: dep_graph.modules()) {
        if(entry.getValue().empty()) {
            continue;
        }
        auto it = selected.find(entry.getKey());
        if(it == selected.end()) {
            changes.appeared.push_back(entry.getKey().str());
        } else if(it->second != entry.getValue().front()) {
            changes.replaced.push_back(it->second);
        }
    }
    return changes;
}

static std::optional<Spelling> database_in(const Spelling& dir) {
    Spelling candidate("compile_commands.json", dir);
    if(!vfs::exists(candidate)) {
        return std::nullopt;
    }
    return candidate;
}

llvm::SmallVector<Spelling> database_places(CanonicalRef workspace_root) {
    llvm::SmallVector<Spelling> places;
    if(workspace_root.empty()) {
        return places;
    }
    places.emplace_back("compile_commands.json", Spelling(workspace_root));

    // Name order, so build/ and out/ side by side load in the same order on
    // every start rather than whichever the directory listing yields first.
    llvm::SmallVector<Spelling> subdirectories;
    for(auto& entry: vfs::read_dir(workspace_root).value_or(std::vector<vfs::Entry>())) {
        // A symlinked build directory is a build directory too, even
        // before its target exists.
        if(entry.type == llvm::sys::fs::file_type::symlink_file ||
           entry.type == llvm::sys::fs::file_type::directory_file) {
            subdirectories.push_back(Spelling::absolute(entry.path));
        }
    }
    std::ranges::sort(subdirectories, {}, &Spelling::str);
    for(auto& subdirectory: subdirectories) {
        places.emplace_back("compile_commands.json", subdirectory);
    }
    return places;
}

llvm::SmallVector<Spelling> discover_compile_commands(CanonicalRef workspace_root) {
    auto found = database_places(workspace_root);
    llvm::erase_if(found, [](const Spelling& place) { return !vfs::exists(place); });
    return found;
}

llvm::SmallVector<Spelling> compile_commands_below(CanonicalRef workspace_root,
                                                   CanonicalRef cache_dir) {
    llvm::SmallVector<Spelling> found;
    vfs::walk(workspace_root, [&](const vfs::Entry& entry) {
        auto spelled = Spelling::absolute(entry.path);
        if(entry.type == llvm::sys::fs::file_type::directory_file) {
            return path::filename(spelled.str()) != ".git" &&
                   workspace_root.entry(spelled) != cache_dir;
        }
        if(path::filename(spelled.str()) == "compile_commands.json") {
            found.push_back(std::move(spelled));
        } else if(entry.type == llvm::sys::fs::file_type::symlink_file &&
                  vfs::is_directory(spelled)) {
            // Not walked into (links may cycle), but a build directory
            // symlinked elsewhere keeps its database.
            if(auto database = database_in(spelled)) {
                found.push_back(std::move(*database));
            }
        }
        return false;
    });
    return found;
}

static bool configured(const Spelling& dir) {
    return llvm::any_of(config_file_names,
                        [&](llvm::StringRef name) { return vfs::exists(Spelling(name, dir)); });
}

bool defines_project(CanonicalRef dir) {
    return configured(Spelling(dir)) || !discover_compile_commands(dir).empty();
}

CanonicalPath project_root_above(CanonicalRef start) {
    CanonicalPath root;
    path::walk_ancestors(start, [&](CanonicalRef dir) {
        Spelling spelled(dir);
        if(configured(spelled) || database_in(spelled) || database_in(Spelling("build", spelled))) {
            root = dir;
            return false;
        }
        return true;
    });
    return root;
}

llvm::SmallVector<Spelling> database_places_above(CanonicalRef start, CanonicalRef workspace_root) {
    llvm::SmallVector<Spelling> places;
    path::walk_ancestors(start, workspace_root, [&](llvm::StringRef dir) {
        places.emplace_back("compile_commands.json", Spelling::absolute(dir));
        return true;
    });
    return places;
}

llvm::SmallVector<Spelling> compile_commands_above(CanonicalRef start,
                                                   CanonicalRef workspace_root) {
    auto found = database_places_above(start, workspace_root);
    llvm::erase_if(found, [](const Spelling& place) { return !vfs::exists(place); });
    return found;
}

DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build".
    auto baseline_before_ns = vfs::stat_baseline_before_ns(build_at);

    DepsSnapshot snap;
    snap.reserve(deps.size());
    vfs::StatusBatch statuses;
    for(const auto& file: deps) {
        auto& dep = snap.emplace_back();
        dep.path_id = files.intern(Spelling::absolute(file.path));
        auto hash = file.hash;

        // A place a failed lookup looked: the build saw nothing there,
        // whatever is there by now. The file table watches it from here on;
        // a file there is a change.
        if(file.absent) {
            dep.missing = true;
            files.current(dep.path_id);
            continue;
        }

        auto status = statuses.status(file.path);
        if(!status) {
            // A file the build read that is gone already: record the
            // absence, reappearing counts as a change. Still-missing
            // deliberately counts as unchanged — flagging it would rebuild
            // on every check without ever converging, while the artifact is
            // the last remaining truth for the file (and dependents'
            // recovery is the DiskRemoved cascade's job, not this
            // snapshot's).
            dep.missing = true;
            files.saw_missing(dep.path_id);
            continue;
        }

        if(hash == 0) {
            if(status->stamp.mtime_ns > baseline_before_ns) {
                // The worker could not hash the consumed bytes and the file
                // may have changed during the build — no version can name
                // them. The dep stays version-less and reads as changed
                // until the rebuild's capture retries.
                continue;
            }
            // The unchanged mtime proves the disk still holds the consumed
            // bytes, so their hash can be taken from the last reliable read
            // — or one read, unless the file moved between the stat and the
            // read, which voids the proof.
            auto obs = files.observe_for(dep.path_id, *status);
            if(!obs || obs->stamp != status->stamp) {
                continue;
            }
            hash = obs->hash;
        } else {
            files.disk.consumed(dep.path_id, hash);
        }

        dep.version = files.intern_version(dep.path_id, hash);
    }
    return snap;
}

bool deps_changed(FileTable& files, const DepsSnapshot& snap) {
    auto changed = [&](const DepState& dep) {
        // Gone at build time: reappearing is the change; still-missing
        // stays unchanged (see the capture).
        if(dep.missing) {
            return files.present(dep.path_id);
        }
        // No version names the consumed bytes: rebuild once to converge.
        if(!dep.version.valid()) {
            return true;
        }
        // Missing means gone now — a change, since the build saw the file.
        // Unreadable cannot prove the disk unchanged and counts as changed
        // — conservative, retried by the rebuild's capture.
        return files.check_version(dep.version) != vfs::DiskState::Verdict::Fresh;
    };
    return std::ranges::count_if(snap, changed) != 0;
}

std::shared_ptr<index::TUIndex> load_pch_envelope(llvm::StringRef path) {
    auto buffer = vfs::read(path, vfs::Read::Mapped);
    if(!buffer) {
        return nullptr;
    }
    // A stale or truncated pair must never crash the server: the envelope
    // is deep-verified, and every embedded shard blob once — queries then
    // run unchecked. Anything failing reads as "pair missing" and the PCH
    // is rebuilt.
    auto envelope = index::TUIndex::from_buffer(std::move(*buffer));
    if(!envelope.loaded() || !envelope.shards_verify()) {
        return nullptr;
    }
    return std::make_shared<index::TUIndex>(std::move(envelope));
}

const std::shared_ptr<index::TUIndex>& PCHState::load_state() {
    if(!state && !index_path.empty()) {
        state = load_pch_envelope(index_path);
        if(!state) {
            // Unreadable blob: clear the path so queries don't retry the
            // mmap + verification on every call. The pair now looks
            // incomplete and ensure_pch rebuilds it on the next compile.
            LOG_WARN("Failed to open pch.idx envelope {}", index_path);
            index_path.clear();
        }
    }
    return state;
}

void Project::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                            Fid exclude_path_id) const {
    for(auto& [pid, st]: pcm_cache) {
        if(pid == exclude_path_id)
            continue;
        auto module_name = dep_graph.module_of(pid);
        if(!module_name.empty()) {
            pcms[module_name.str()] = st.path;
        }
    }
}

}  // namespace clice
