#include "project/project.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <tuple>

#include "command/search_config.h"
#include "index/serialization.h"
#include "project/hosting.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/preamble_synthesis.h"
#include "syntax/scan.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

std::uint32_t Project::count_occurrences(Fid host_id, Fid target_id) const {
    auto chain = dep_graph.find_include_chain(host_id, target_id);
    if(chain.size() < 2) {
        return 0;
    }
    auto includer_path = file_table.resolve(chain[chain.size() - 2]);
    auto target_path = file_table.resolve(target_id);
    auto buf = llvm::MemoryBuffer::getFile(includer_path);
    if(!buf) {
        return 0;
    }
    auto null_resolver =
        [](llvm::StringRef, bool, bool, llvm::StringRef) -> std::optional<std::string> {
        return std::nullopt;
    };
    return count_include_occurrences(without_bom((*buf)->getBuffer()),
                                     includer_path,
                                     target_path,
                                     null_resolver);
}

void Project::rescan_disk_file(Fid path_id) {
    auto path = file_table.resolve(path_id);
    dep_graph.clear_includes(path_id);

    // One read serves everything a save invalidates: the shared pair (so
    // hash comparisons elsewhere stop re-reading), the lexical scan
    // (include edges and the module declaration), and the bytes the
    // module-decl preprocessor fallback must consume.
    auto observed = read_file_observed(path.data());
    if(observed) {
        file_table.observe(path_id, observed->obs);
        const auto& scan =
            file_table.scan_of(path_id, observed->obs.hash, observed->content->getBuffer());

        // Search paths come from the file's effective commands, or a host's
        // for headers without one (the header's own edits on top, as its
        // compile applies them); the builtin fallback still resolves quote
        // includes via the includer directory. Every command contributes
        // its own edges, as the startup scan does.
        Fid cmd_file = path_id;
        CanonicalRef cmd_path = path;
        std::optional<Lender> lender;
        if(!build.unit(path_id)) {
            if(auto host = default_host(*this, path_id)) {
                cmd_file = host->file;
                cmd_path = file_table.resolve(host->file);
            } else if(build.commands(path_id).empty()) {
                if(lender = command_lender(*this, path_id); lender) {
                    cmd_path = file_table.resolve(lender->unit);
                }
            }
        }

        llvm::SmallVector<CommandRef, 2> refs;
        if(lender) {
            refs.push_back(build.resolve(path_id,
                                         lender->config,
                                         CommandSource::Inferred,
                                         {cmd_path, path},
                                         cmd_path));
        }
        for(auto& command: lender ? llvm::SmallVector<Candidate, 2>{} : build.commands(cmd_file)) {
            refs.push_back(
                build.resolve(path_id, command.config, command.source, {cmd_path, path}, cmd_path));
        }
        if(refs.empty()) {
            refs.push_back(
                build.resolve(path_id, build.builtin(path), CommandSource::Fallback, path, path));
        }

        DirListingCache dir_cache;
        dir_cache.shared = &file_table;
        auto spelled_dir = file_table.spelling(path_id).parent();
        llvm::StringRef dir = spelled_dir;
        auto entries = resolve_dir(dir, dir_cache);
        for(auto [index, ref]: llvm::enumerate(refs)) {
            auto search_config = cdb.search_config(ref);
            auto resolved_config = resolve_search_config(search_config, dir_cache);
            llvm::SmallVector<IncludeEdge> edges;
            for(auto& include: scan.includes) {
                auto resolved = resolve_include(include.path,
                                                include.is_angled,
                                                entries,
                                                dir,
                                                include.is_include_next,
                                                0,
                                                resolved_config,
                                                dir_cache);
                if(resolved) {
                    auto found = Spelling::absolute(resolved->path);
                    auto included = file_table.intern(found);
                    file_table.spell_as(included, found);
                    edges.push_back({included, include.conditional});
                }
            }
            dep_graph.set_includes(path_id, static_cast<std::uint32_t>(index), std::move(edges));
        }

        context_epoch += 1;

        // The graph's module declaration is what import resolution reads —
        // left stale, an interface saved mid-session could never satisfy
        // its importers.
        auto module_name = scan.module_name;
        bool is_interface_unit = scan.is_interface_unit;
        // A module declaration inside a preprocessor conditional is beyond
        // the lexical scan (need_preprocess, name left empty): resolve it
        // with the same scan_module_decl() fallback the startup scan uses,
        // or this save would drop a guarded interface from both provider
        // maps and leave its importers unresolved until a reload.
        if(scan.need_preprocess) {
            // Under the default selection, as the startup scan preprocesses
            // each unit under its own first command.
            auto& ref = refs.front();
            auto rendered = cdb.render(ref);
            llvm::SmallString<512> joined;
            for(auto* arg: rendered) {
                joined.append(arg);
                joined.push_back('\0');
            }
            auto key = std::pair{observed->obs.hash, llvm::xxh3_64bits(joined)};
            auto cached = file_table.module_decls.find(key);
            if(cached == file_table.module_decls.end()) {
                // The preprocessor consumes the very bytes that produced
                // the scan; negative results memoize too.
                auto fallback = scan_module_decl(rendered,
                                                 cdb.config(ref.config).directory,
                                                 observed->content->getBuffer());
                cached = file_table.module_decls
                             .try_emplace(key,
                                          FileTable::ModuleDecl{fallback.module_name,
                                                                fallback.is_interface_unit})
                             .first;
            }
            if(!cached->second.name.empty()) {
                module_name = cached->second.name;
                is_interface_unit = cached->second.is_interface_unit;
            }
        }
        // Interface units only, mirroring the startup scan: an
        // implementation unit (`module foo;`) must never satisfy
        // lookup_module — importers would edge to it and try to build it
        // as an interface — nor claim a PCM node of its own.
        if(!is_interface_unit) {
            module_name.clear();
        }
        dep_graph.update_module_decl(path_id, module_name);
        dep_graph.set_import_candidate(path_id, scan.has_import);
        return;
    }

    context_epoch += 1;
}

void Project::forget_file(Fid path_id) {
    dep_graph.update_module_decl(path_id, {});
    dep_graph.set_import_candidate(path_id, false);
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
    dep_graph = DependencyGraph();
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
    if(!llvm::sys::fs::exists(candidate)) {
        return std::nullopt;
    }
    return candidate;
}

llvm::SmallVector<Spelling> discover_compile_commands(CanonicalRef workspace_root) {
    llvm::SmallVector<Spelling> found;
    if(workspace_root.empty()) {
        return found;
    }
    Spelling root(workspace_root);
    if(auto database = database_in(root)) {
        found.push_back(std::move(*database));
    }

    // Name order, so build/ and out/ side by side load in the same order on
    // every start rather than whichever the directory listing yields first.
    llvm::SmallVector<Spelling> subdirectories;
    std::error_code ec;
    for(llvm::sys::fs::directory_iterator it(workspace_root, ec), end; it != end && !ec;
        it.increment(ec)) {
        // A symlinked build directory is a build directory too.
        if(llvm::sys::fs::is_directory(it->path())) {
            subdirectories.push_back(Spelling::absolute(it->path()));
        }
    }
    std::ranges::sort(subdirectories, {}, &Spelling::str);
    for(auto& subdirectory: subdirectories) {
        if(auto database = database_in(subdirectory)) {
            found.push_back(std::move(*database));
        }
    }
    return found;
}

llvm::SmallVector<Spelling> compile_commands_below(CanonicalRef workspace_root,
                                                   CanonicalRef cache_dir) {
    llvm::SmallVector<Spelling> found;
    std::error_code ec;
    for(llvm::sys::fs::recursive_directory_iterator
            it(workspace_root, ec, /*follow_symlinks=*/false),
        end;
        it != end;
        it.increment(ec)) {
        if(ec) {
            LOG_WARN("Cannot read a directory under {}: {}", workspace_root, ec.message());
            ec.clear();
            continue;
        }
        auto entry = Spelling::absolute(it->path());
        auto type = it->type();
        if(type == llvm::sys::fs::file_type::directory_file) {
            if(path::filename(entry.str()) == ".git" || workspace_root.entry(entry) == cache_dir) {
                it.no_push();
            }
        } else if(path::filename(entry.str()) == "compile_commands.json") {
            found.push_back(std::move(entry));
        } else if(type == llvm::sys::fs::file_type::symlink_file &&
                  llvm::sys::fs::is_directory(entry)) {
            // Not walked into (links may cycle), but a build directory
            // symlinked elsewhere keeps its database.
            if(auto database = database_in(entry)) {
                found.push_back(std::move(*database));
            }
        }
    }
    return found;
}

static bool configured(const Spelling& dir) {
    return llvm::any_of(config_file_names, [&](llvm::StringRef name) {
        return llvm::sys::fs::exists(Spelling(name, dir));
    });
}

bool defines_project(CanonicalRef dir) {
    return configured(Spelling(dir)) || !discover_compile_commands(dir).empty();
}

CanonicalPath project_root_above(CanonicalRef start) {
    for(CanonicalPath dir = start; !dir.empty();) {
        Spelling spelled(dir);
        if(configured(spelled) || database_in(spelled) || database_in(Spelling("build", spelled))) {
            return dir;
        }
        auto parent = CanonicalRef(dir).parent();
        if(parent.size() == dir.size()) {
            break;
        }
        dir = std::move(parent);
    }
    return {};
}

llvm::SmallVector<Spelling> compile_commands_above(CanonicalRef start,
                                                   CanonicalRef workspace_root) {
    llvm::SmallVector<Spelling> found;
    path::walk_ancestors(start, workspace_root, [&](llvm::StringRef dir) {
        if(auto database = database_in(Spelling::absolute(dir))) {
            found.push_back(std::move(*database));
        }
        return true;
    });
    return found;
}

DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build".
    auto baseline_before_ns = fs::stat_baseline_before_ns(build_at);

    DepsSnapshot snap;
    snap.reserve(deps.size());
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

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(file.path, status)) {
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

        auto size = status.getSize();
        auto mtime_ns = fs::mtime_ns(status);
        if(hash == 0) {
            if(mtime_ns > baseline_before_ns) {
                // The worker could not hash the consumed bytes and the file
                // may have changed during the build — no version can name
                // them. The dep stays version-less and reads as changed
                // until the rebuild's capture retries.
                continue;
            }
            // The unchanged mtime proves the disk still holds the consumed
            // bytes, so their hash can be taken from the shared pair — or
            // one read, unless the file moved between the stat and the
            // read, which voids the proof.
            auto obs = files.observe_for(dep.path_id, status);
            if(!obs || obs->size != size || obs->mtime_ns != mtime_ns) {
                continue;
            }
            hash = obs->hash;
        }

        dep.version = files.intern_version(dep.path_id, hash);
    }
    return snap;
}

bool deps_changed(FileTable& files, const DepsSnapshot& snap) {
    for(auto& dep: snap) {
        if(dep.missing) {
            // Gone at build time: reappearing is the change; still-missing
            // stays unchanged (see the capture).
            if(files.current(dep.path_id)) {
                return true;
            }
            continue;
        }

        // No version names the consumed bytes: rebuild once to converge.
        if(!dep.version.valid()) {
            return true;
        }

        // Missing means gone now — a change, since the build saw the file.
        // Unreadable cannot prove the disk unchanged and counts as changed
        // — conservative, retried by the rebuild's capture.
        if(files.check_version(dep.version) != FileTable::Verdict::Fresh) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<index::TUIndex> load_pch_envelope(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
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
