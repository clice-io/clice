module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :project.hosting;
import :project.project;
import :support.logging;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice {

void Project::rescan_disk_file(Fid path_id) {
    rescan_dependency_graph(cdb, toolchain, dep_graph, path_id);
    context_epoch += 1;
}

void Project::forget_file(Fid path_id) {
    dep_graph.update_module_decl(path_id, {});
    dep_graph.forget_scan(path_id);
    dep_graph.clear_includes(path_id);
    context_epoch += 1;
}

Project::Rebuilt Project::rebuild_dependency_graph() {
    llvm::StringMap<llvm::SmallVector<Fid, 2>> providers;
    for(auto& entry: dep_graph.modules()) {
        if(!entry.getValue().empty()) {
            providers[entry.getKey()] = entry.getValue();
        }
    }

    // TODO: every CDB change rescans the whole graph synchronously on the
    // event loop, twice when provisional members change; make it
    // incremental or move it off the dispatch path.
    auto scan = [&] {
        dep_graph.reset();
        scan_dependency_graph(cdb, toolchain, dep_graph, build.units(build.members()));
        dep_graph.build_reverse_map();
    };
    scan();
    Rebuilt changes{.provisional = refresh_provisional()};
    if(!changes.provisional.empty()) {
        scan();
    }
    context_epoch += 1;

    for(auto& entry: dep_graph.modules()) {
        if(entry.getValue().empty()) {
            continue;
        }
        auto it = providers.find(entry.getKey());
        if(it == providers.end()) {
            changes.appeared.push_back(entry.getKey().str());
            continue;
        }
        auto now = ranges::to<llvm::SmallVector<Fid>>(entry.getValue());
        ranges::sort(it->second);
        ranges::sort(now);
        if(it->second != now) {
            changes.replaced.append(it->second.begin(), it->second.end());
        }
    }
    return changes;
}

CDBDiff Project::refresh_provisional() {
    CDBDiff diff;
    auto recorded = ranges::to<llvm::SmallVector<Fid>>(build.recorded());
    ranges::sort(recorded, {}, [&](Fid file) { return file_table.resolve(file); });
    for(auto file: recorded) {
        if(build.declared(file)) {
            build.forget(file);
            continue;
        }
        // A file a declared unit includes compiles in its includer's
        // context, as the part of a unity build does.
        auto hosted = ranges::any_of(dep_graph.find_host_sources(file), [&](Fid root) {
            return root != file && build.declared(root);
        });
        std::optional<CommandID> command;
        if(!hosted) {
            if(auto lender = command_lender(*this, file)) {
                command = build.lend(lender->command,
                                     file_table.resolve(lender->unit),
                                     file_table.resolve(file));
            }
        }
        auto before = build.borrow(file, command);
        if(before == command) {
            continue;
        }
        (!before ? diff.added : !command ? diff.removed : diff.changed).push_back(file);
    }
    return diff;
}

namespace {

constexpr llvm::StringLiteral database_name = "compile_commands.json";

std::optional<Spelling> database_in(const Spelling& dir) {
    Spelling candidate(database_name, dir);
    if(!vfs::exists(candidate)) {
        return std::nullopt;
    }
    return candidate;
}

/// The places that hold a database.
llvm::SmallVector<Spelling> existing(llvm::SmallVector<Spelling> places) {
    places.erase(
        ranges::remove_if(places, [](const Spelling& place) { return !vfs::exists(place); })
            .begin(),
        places.end());
    return places;
}

bool configured(const Spelling& dir) {
    return ranges::any_of(config_file_names,
                          [&](llvm::StringRef name) { return vfs::exists(Spelling(name, dir)); });
}

}  // namespace

llvm::SmallVector<Spelling> database_places(CanonicalRef workspace_root) {
    llvm::SmallVector<Spelling> places;
    if(workspace_root.empty()) {
        return places;
    }
    places.emplace_back(database_name, Spelling(workspace_root));

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
    ranges::sort(subdirectories, {}, &Spelling::str);
    for(auto& subdirectory: subdirectories) {
        places.emplace_back(database_name, subdirectory);
    }
    return places;
}

llvm::SmallVector<Spelling> discover_compile_commands(CanonicalRef workspace_root) {
    return existing(database_places(workspace_root));
}

llvm::SmallVector<Spelling> deeper_compile_commands(CanonicalRef workspace_root,
                                                    CanonicalRef cache_dir) {
    // Deep enough for a CMake preset's out/build/<preset> and a nested
    // project's build directory, and no deeper: a workspace with no
    // database at all may be a home directory.
    constexpr std::size_t deepest = 3;
    auto depth = [&](llvm::StringRef dir) {
        return static_cast<std::size_t>(
            ranges::count_if(dir.drop_front(workspace_root.size()),
                             [](char c) { return path::is_separator(c); }));
    };
    llvm::SmallVector<Spelling> found;
    vfs::walk(workspace_root, [&](const vfs::Entry& entry) {
        auto spelled = Spelling::absolute(entry.path);
        if(entry.type == llvm::sys::fs::file_type::directory_file) {
            auto name = path::filename(spelled.str());
            return !name.starts_with(".") && name != "node_modules" &&
                   workspace_root.entry(spelled) != cache_dir && depth(spelled) <= deepest;
        }
        // The root and its direct subdirectories are where discovery looks.
        if(path::filename(spelled.str()) == database_name) {
            if(depth(spelled.parent()) >= 2) {
                found.push_back(std::move(spelled));
            }
        } else if(entry.type == llvm::sys::fs::file_type::symlink_file &&
                  vfs::is_directory(spelled) && depth(spelled) >= 2) {
            // Not walked into (links may cycle), but a build directory
            // symlinked elsewhere keeps its database.
            if(auto database = database_in(spelled)) {
                found.push_back(std::move(*database));
            }
        }
        return false;
    });
    ranges::sort(found, {}, &Spelling::str);
    return found;
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
        places.emplace_back(database_name, Spelling::absolute(dir));
        return true;
    });
    return places;
}

llvm::SmallVector<Spelling> compile_commands_above(CanonicalRef start,
                                                   CanonicalRef workspace_root) {
    return existing(database_places_above(start, workspace_root));
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
            files.disk.current(dep.path_id);
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
            files.disk.saw_missing(dep.path_id);
            continue;
        }

        if(hash == 0) {
            // The worker could not hash the consumed bytes: when nothing
            // else names them, the dep stays version-less and reads as
            // changed until the rebuild's capture retries.
            hash = files.disk.unchanged_hash(dep.path_id, *status, baseline_before_ns);
            if(hash == 0) {
                continue;
            }
        } else {
            files.disk.consumed(dep.path_id, hash);
        }

        dep.version = files.intern_version(dep.path_id, hash);
    }
    return snap;
}

void merge_deps(DepsSnapshot& into, llvm::ArrayRef<const DepsSnapshot*> parts) {
    auto key = [](const DepState& dep) {
        return std::tuple(dep.path_id.raw, dep.version.raw, std::uint8_t(dep.missing));
    };
    llvm::DenseSet<std::tuple<std::uint32_t, std::uint32_t, std::uint8_t>> seen;
    for(auto& dep: into) {
        seen.insert(key(dep));
    }
    for(auto* part: parts) {
        for(auto& dep: *part) {
            if(seen.insert(key(dep)).second) {
                into.push_back(dep);
            }
        }
    }
}

bool deps_changed(FileTable& files, const DepsSnapshot& snap) {
    auto changed = [&](const DepState& dep) {
        // Gone at build time: reappearing is the change; still-missing
        // stays unchanged (see the capture).
        if(dep.missing) {
            return files.disk.present(dep.path_id);
        }
        // No version names the consumed bytes: rebuild once to converge.
        if(!dep.version.valid()) {
            return true;
        }
        // Missing means gone now — a change, since the build saw the file.
        // Unreadable cannot prove the disk unchanged and counts as changed
        // — conservative, retried by the rebuild's capture.
        return files.check_version(dep.version) != vfs::DiskState::CheckResult::Fresh;
    };
    return ranges::count_if(snap, changed) != 0;
}

std::shared_ptr<index::TUIndex> load_pch_index(llvm::StringRef path) {
    auto buffer = vfs::read(path, vfs::Read::Mapped);
    if(!buffer) {
        return nullptr;
    }
    // A stale or truncated pair must never crash the server: the TU index
    // is deep-verified, and every embedded shard blob once — queries then
    // run unchecked. Anything failing reads as "pair missing" and the PCH
    // is rebuilt.
    auto tu_index = index::TUIndex::from_buffer(std::move(*buffer));
    if(!tu_index.loaded() || !tu_index.verify_shards()) {
        return nullptr;
    }
    return std::make_shared<index::TUIndex>(std::move(tu_index));
}

const std::shared_ptr<index::TUIndex>& PCHState::load_state() {
    if(!state && !index_path.empty()) {
        state = load_pch_index(index_path);
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

const DepsSnapshot* Project::build_inputs(Fid module) const {
    llvm::SmallVector<Fid, 4> chain;
    for(auto current = module; !ranges::contains(chain, current);) {
        auto it = pcm_builds.find(current);
        if(it == pcm_builds.end()) {
            return nullptr;
        }
        auto& build = it->second;
        if(!build.failure || !build.failure->import.valid()) {
            return &build.deps;
        }
        chain.push_back(current);
        current = build.failure->import;
    }
    return nullptr;
}

DepsSnapshot Project::module_inputs(llvm::ArrayRef<Fid> modules) const {
    llvm::SmallVector<const DepsSnapshot*> parts;
    for(auto module: modules) {
        if(auto* inputs = build_inputs(module)) {
            parts.push_back(inputs);
        }
    }
    DepsSnapshot inputs;
    merge_deps(inputs, parts);
    return inputs;
}

void Project::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                            llvm::ArrayRef<Fid> modules) const {
    for(auto module: modules) {
        auto it = pcm_builds.find(module);
        auto name = dep_graph.module_of(module);
        if(it != pcm_builds.end() && !it->second.path.empty() && !name.empty()) {
            pcms[name.str()] = it->second.path;
        }
    }
}

}  // namespace clice
