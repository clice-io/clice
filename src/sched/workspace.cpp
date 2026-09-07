#include "sched/workspace.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <tuple>

#include "command/search_config.h"
#include "index/serialization.h"
#include "sched/context.h"
#include "sched/hosting.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/preamble_synthesis.h"
#include "syntax/scan.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

bool Workspace::is_synthesized_artifact(llvm::StringRef path) const {
    if(!store) {
        return false;
    }
    return path.starts_with(path::join(store->base_dir(), header_context_ns));
}

std::uint32_t Workspace::count_occurrences(Fid host_id, Fid target_id) const {
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
    return count_include_occurrences((*buf)->getBuffer(),
                                     includer_path,
                                     target_path,
                                     null_resolver);
}

void Workspace::rescan_after_save(Fid path_id) {
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
        llvm::StringRef cmd_path = path;
        std::optional<Lender> lender;
        if(!build.unit(path_id)) {
            if(auto host = default_host(*this, path_id)) {
                cmd_file = host->file;
                cmd_path = file_table.resolve(host->file);
            } else if(lender = command_lender(*this, path_id); lender) {
                cmd_path = file_table.resolve(lender->unit);
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
        auto dir = llvm::sys::path::parent_path(path);
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
                    edges.push_back({file_table.intern(resolved->path), include.conditional});
                }
            }
            dep_graph.set_includes(path_id, static_cast<std::uint32_t>(index), std::move(edges));
        }

        dep_graph.build_reverse_map();
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

    dep_graph.build_reverse_map();
    context_epoch += 1;
}

void Workspace::on_file_closed(Fid path_id) {
    // PCH entries are content-keyed and may be shared with other sessions,
    // so nothing entry-level to clean up — but the loaded-state budget
    // shrinks with the open count, and this is the moment it does.
    enforce_loaded_budget();
}

static std::string database_in(llvm::StringRef dir) {
    auto candidate = path::join(dir, "compile_commands.json");
    return llvm::sys::fs::exists(candidate) ? candidate : std::string();
}

llvm::SmallVector<std::string> discover_compile_commands(llvm::StringRef workspace_root) {
    llvm::SmallVector<std::string> found;
    if(workspace_root.empty()) {
        return found;
    }
    if(auto database = database_in(workspace_root); !database.empty()) {
        found.push_back(std::move(database));
    }

    // Name order, so build/ and out/ side by side load in the same order on
    // every start rather than whichever the directory listing yields first.
    llvm::SmallVector<std::string> subdirectories;
    std::error_code ec;
    for(llvm::sys::fs::directory_iterator it(workspace_root, ec), end; it != end && !ec;
        it.increment(ec)) {
        if(it->type() == llvm::sys::fs::file_type::directory_file) {
            subdirectories.push_back(it->path());
        }
    }
    std::ranges::sort(subdirectories);
    for(auto& subdirectory: subdirectories) {
        if(auto database = database_in(subdirectory); !database.empty()) {
            found.push_back(std::move(database));
        }
    }
    return found;
}

llvm::SmallVector<std::string> compile_commands_below(llvm::StringRef workspace_root,
                                                      llvm::StringRef cache_dir) {
    llvm::SmallVector<std::string> found;
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
        llvm::SmallString<256> storage;
        auto entry_path = path::canonical(it->path(), storage);
        if(it->type() == llvm::sys::fs::file_type::directory_file) {
            if(path::filename(entry_path) == ".git" || entry_path == cache_dir) {
                it.no_push();
            }
        } else if(path::filename(entry_path) == "compile_commands.json") {
            found.push_back(entry_path.str());
        }
    }
    return found;
}

llvm::SmallVector<std::string> compile_commands_above(llvm::StringRef start,
                                                      llvm::StringRef workspace_root) {
    llvm::SmallVector<std::string> found;
    path::walk_ancestors(start, workspace_root, [&](llvm::StringRef dir) {
        if(auto database = database_in(dir); !database.empty()) {
            found.push_back(std::move(database));
        }
        return true;
    });
    return found;
}

DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build" and offer no fast-path
    // baseline; one passing hash comparison repairs them (check_version).
    auto baseline_before_ns = fs::stat_baseline_before_ns(build_at);

    DepsSnapshot snap;
    snap.reserve(deps.size());
    for(const auto& file: deps) {
        auto& dep = snap.emplace_back();
        dep.path_id = files.intern(file.path);
        auto hash = file.hash;

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(file.path, status)) {
            // The build read it, but it is gone already: record the absence,
            // reappearing counts as a change. Still-missing deliberately
            // counts as unchanged — flagging it would rebuild on every
            // check without ever converging, while the artifact is the
            // last remaining truth for the file (and dependents' recovery
            // is the DiskRemoved cascade's job, not this snapshot's).
            dep.missing = true;
            continue;
        }

        auto size = status.getSize();
        auto mtime_ns = fs::mtime_ns(status);
        bool untouched = mtime_ns <= baseline_before_ns;
        if(hash == 0) {
            if(!untouched) {
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
        if(untouched) {
            // Untouched since before the build started — the disk still
            // holds the consumed bytes, so the stat is a trustworthy fast
            // path (recorded only when corroborated, see try_stamp).
            auto uid = status.getUniqueID();
            files.try_stamp(dep.version, size, mtime_ns, uid.getDevice(), uid.getFile());
        }
    }
    return snap;
}

bool deps_changed(FileTable& files, const DepsSnapshot& snap) {
    for(auto& dep: snap) {
        if(dep.missing) {
            // Gone at build time: reappearing is the change; still-missing
            // stays unchanged (see the capture).
            if(fs::exists(files.resolve(dep.path_id))) {
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

void force_revalidate_deps(FileTable& files, const DepsSnapshot& snap) {
    for(auto& dep: snap) {
        files.force_revalidate(dep.path_id);
    }
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

std::shared_ptr<index::TUIndex> Workspace::preamble_state(llvm::StringRef pch_key) {
    auto it = pch_cache.find(pch_key);
    if(it == pch_cache.end()) {
        return nullptr;
    }

    auto& st = it->second;
    bool had_blob = !st.index_path.empty();
    auto state = st.load_state();
    if(!state && had_blob && store) {
        // The blob was just found unreadable (load_state cleared the
        // path): a pair that looks complete on disk but cannot be opened
        // would be served to every session for the rest of the store's
        // life. Retract it now; the entry itself stays until ensure_pch
        // re-checks the store and rebuilds the pair.
        LOG_WARN("Retracting PCH pair {} with unreadable pch.idx envelope", pch_key);
        store->invalidate("pch", pch_key);
    }
    if(state) {
        touch_loaded_state(pch_key);
        enforce_loaded_budget();
    }
    return state;
}

void Workspace::touch_loaded_state(llvm::StringRef pch_key) {
    auto it = std::ranges::find(loaded_state_lru, pch_key);
    if(it != loaded_state_lru.end()) {
        loaded_state_lru.erase(it);
    }
    loaded_state_lru.insert(loaded_state_lru.begin(), pch_key.str());
}

void Workspace::enforce_loaded_budget() {
    // Two extra slots over the open-document count: a closed file's
    // recently used state survives a quick close/reopen, and a shared key
    // serving several documents stays warm while its consumers churn.
    // Open documents' keys always fit the budget, so an unload can only
    // hit keys past the working set; the reload an unlucky consumer then
    // pays (mmap + verification, on the event loop) is the accepted cost
    // of bounding tens of MB per key.
    // Unwired (tests, tools) assumes a small editor-like working set.
    constexpr std::size_t default_open_documents = 6;
    std::size_t budget = 2 + (open_documents ? open_documents() : default_open_documents);

    std::size_t kept = 0;
    std::size_t i = 0;
    while(i < loaded_state_lru.size()) {
        auto it = pch_cache.find(loaded_state_lru[i]);
        // Erased entries and already-unloaded keys just fall out of the
        // list (invalidation and store eviction bypass the LRU).
        if(it == pch_cache.end() || !it->second.state) {
            loaded_state_lru.erase(loaded_state_lru.begin() + i);
            continue;
        }
        if(kept < budget) {
            kept += 1;
            i += 1;
            continue;
        }
        LOG_DEBUG("Unloading pch.idx envelope of {} (budget {})", loaded_state_lru[i], budget);
        it->second.state.reset();
        loaded_state_lru.erase(loaded_state_lru.begin() + i);
    }
}

void Workspace::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
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
