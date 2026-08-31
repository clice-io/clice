#include "sched/workspace.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <tuple>

#include "command/search_config.h"
#include "index/serialization.h"
#include "sched/context.h"
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
    if(config.project.cache_dir.empty()) {
        return false;
    }
    auto artifact_dir = path::join(config.project.cache_dir, "header_context");
    return path.starts_with(artifact_dir);
}

std::uint32_t Workspace::count_occurrences(std::uint32_t host_id, std::uint32_t target_id) const {
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

llvm::SmallVector<std::uint32_t> Workspace::rank_hosts(std::uint32_t header_path_id,
                                                       llvm::ArrayRef<std::uint32_t> hosts) const {
    auto header_path = file_table.resolve(header_path_id);
    auto header_stem = llvm::sys::path::stem(header_path);
    auto header_dir = llvm::sys::path::parent_path(header_path);

    auto score = [&](std::uint32_t host_id) -> std::tuple<int, int, std::size_t> {
        auto host_path = file_table.resolve(host_id);
        int stem_match = llvm::sys::path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = llvm::sys::path::parent_path(host_path) == header_dir ? 0 : 1;
        // Longer shared prefix means "closer" in the tree; negate for
        // ascending sort.
        std::size_t common = 0;
        auto n = std::min(host_path.size(), header_path.size());
        while(common < n && host_path[common] == header_path[common]) {
            ++common;
        }
        return {stem_match, same_dir, n - common};
    };

    llvm::SmallVector<std::uint32_t> ranked(hosts.begin(), hosts.end());
    std::ranges::sort(ranked, [&](std::uint32_t a, std::uint32_t b) {
        auto sa = score(a), sb = score(b);
        if(sa != sb) {
            return sa < sb;
        }
        return file_table.resolve(a) < file_table.resolve(b);
    });
    return ranked;
}

void Workspace::rescan_after_save(std::uint32_t path_id) {
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

        // Search paths come from the file's own command, or a host's for
        // headers without a CDB entry; the synthesized default still
        // resolves quote includes via the includer directory.
        llvm::StringRef cmd_path = path;
        if(!cdb.has_entry(path)) {
            for(auto host: rank_hosts(path_id, dep_graph.find_host_sources(path_id))) {
                auto host_path = file_table.resolve(host);
                if(cdb.has_entry(host_path)) {
                    cmd_path = host_path;
                    break;
                }
            }
        }

        std::vector<std::string> rule_append, rule_remove;
        config.match_rules(cmd_path, rule_append, rule_remove);
        auto candidates = cdb.candidate_entries(cmd_path);
        llvm::SmallVector<CommandRef> refs;
        for(auto& entry: candidates) {
            auto applied =
                cdb.apply_rules(entry.config, {.remove = rule_remove, .append = rule_append});
            refs.push_back(
                {entry.file, applied, cdb.input_kind(applied, cmd_path), CommandSource::CDBExact});
        }
        if(refs.empty()) {
            auto fallback = cdb.fallback_config(cmd_path);
            auto applied =
                cdb.apply_rules(fallback, {.remove = rule_remove, .append = rule_append});
            refs.push_back(
                {path_id, applied, cdb.input_kind(applied, cmd_path), CommandSource::Fallback});
        }

        // Resolve under every configuration: an include may only be
        // reachable through the -I set of a non-first CDB entry. The local
        // index serves as config id — prior keys were just cleared and
        // consumers read the union.
        DirListingCache dir_cache;
        dir_cache.shared = &file_table;
        auto dir = llvm::sys::path::parent_path(path);
        for(std::uint32_t ci = 0; ci < refs.size(); ++ci) {
            auto search_config = cdb.search_config(refs[ci]);
            auto resolved_config = resolve_search_config(search_config, dir_cache);
            auto entries = resolve_dir(dir, dir_cache);

            llvm::SmallVector<std::uint32_t> ids;
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
                    ids.push_back(file_table.intern(resolved->path));
                }
            }
            dep_graph.set_includes(path_id, ci, std::move(ids));
        }

        dep_graph.build_reverse_map();
        context_epoch += 1;

        // Update both module maps: path_to_module gates the module code
        // paths, and the dep_graph side is what import resolution reads —
        // left stale, an interface saved mid-session could never satisfy
        // its importers.
        auto module_name = scan.module_name;
        bool is_interface_unit = scan.is_interface_unit;
        // A module declaration inside a preprocessor conditional is beyond
        // the lexical scan (need_preprocess, name left empty): resolve it
        // with the same scan_module_decl() fallback the startup scan uses,
        // or this save would drop a guarded interface from both provider
        // maps and leave its importers unresolved until a reload.
        if(scan.need_preprocess && !refs.empty()) {
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
        // Both maps hold interface units only, mirroring the startup scan:
        // an implementation unit (`module foo;`) must never satisfy
        // lookup_module — importers would edge to it and try to build it
        // as an interface — nor claim a PCM node of its own.
        if(!is_interface_unit) {
            module_name.clear();
        }
        dep_graph.update_module_decl(path_id, module_name);
        dep_graph.set_import_candidate(path_id, scan.has_import);
        if(!module_name.empty()) {
            path_to_module[path_id] = std::move(module_name);
        } else {
            path_to_module.erase(path_id);
        }
        return;
    }

    dep_graph.build_reverse_map();
    context_epoch += 1;
}

void Workspace::on_file_closed(std::uint32_t path_id) {
    // PCH entries are content-keyed and may be shared with other sessions,
    // so nothing entry-level to clean up — but the loaded-state budget
    // shrinks with the open count, and this is the moment it does.
    enforce_loaded_budget();
}

std::string discover_compile_commands(const Config& config, llvm::StringRef workspace_root) {
    for(auto& configured: config.project.compile_commands_paths) {
        if(llvm::sys::fs::is_directory(configured)) {
            auto candidate = path::join(configured, "compile_commands.json");
            if(llvm::sys::fs::exists(candidate)) {
                return candidate;
            }
        } else if(llvm::sys::fs::exists(configured)) {
            return configured;
        } else {
            LOG_DEBUG("Configured compile_commands_path not found: {}", configured);
        }
    }

    if(workspace_root.empty()) {
        return {};
    }

    auto try_candidate = [](llvm::StringRef dir) -> std::string {
        auto candidate = path::join(dir, "compile_commands.json");
        if(llvm::sys::fs::exists(candidate)) {
            return candidate;
        }
        return {};
    };

    if(auto found = try_candidate(workspace_root); !found.empty()) {
        return found;
    }

    std::error_code ec;
    for(llvm::sys::fs::directory_iterator it(workspace_root, ec), end; it != end && !ec;
        it.increment(ec)) {
        if(it->type() == llvm::sys::fs::file_type::directory_file) {
            if(auto found = try_candidate(it->path()); !found.empty()) {
                return found;
            }
        }
    }
    return {};
}

DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build" and offer no fast-path
    // baseline; one passing hash comparison repairs them (check_version).
    auto baseline_before_ns = fs::stat_baseline_before_ns(build_at);

    DepsSnapshot snap;
    snap.deps.reserve(deps.size());
    for(const auto& file: deps) {
        auto& dep = snap.deps.emplace_back();
        dep.path_id = files.intern(file.path);
        dep.hash = file.hash;

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(file.path, status)) {
            // The build read it, but it is gone already: record the absence,
            // reappearing counts as a change. Still-missing deliberately
            // counts as unchanged — flagging it would rebuild on every
            // check without ever converging, while the artifact is the
            // last remaining truth for the file (and dependents' recovery
            // is the DiskRemoved cascade's job, not this snapshot's).
            dep.missing = true;
            dep.hash = 0;
            continue;
        }

        auto size = status.getSize();
        auto mtime_ns = fs::mtime_ns(status);
        bool untouched = mtime_ns <= baseline_before_ns;
        if(dep.hash == 0) {
            if(!untouched) {
                // The worker could not hash the consumed bytes and the file
                // may have changed during the build — no version can name
                // them. The zero hash reads as changed and the rebuild's
                // capture retries.
                continue;
            }
            // The unchanged mtime proves the disk still holds the consumed
            // bytes, so their hash can be taken from the shared pair — or
            // one read, unless the file moved between the stat and the
            // read, which voids the proof.
            auto obs = files.observe_for(dep.path_id, size, mtime_ns);
            if(!obs || obs->size != size || obs->mtime_ns != mtime_ns) {
                continue;
            }
            dep.hash = obs->hash;
        }

        auto vid = files.intern_version(dep.path_id, dep.hash);
        if(untouched) {
            // Untouched since before the build started — the disk still
            // holds the consumed bytes, so the stat is a trustworthy fast
            // path (recorded only when corroborated, see try_stamp).
            files.try_stamp(vid, size, mtime_ns);
        }
    }
    return snap;
}

bool deps_changed(FileTable& files, const DepsSnapshot& snap) {
    for(auto& dep: snap.deps) {
        if(dep.missing) {
            // Gone at build time: reappearing is the change; still-missing
            // stays unchanged (see the capture).
            if(fs::exists(files.resolve(dep.path_id))) {
                return true;
            }
            continue;
        }

        // No trusted hash to compare against: rebuild once to converge.
        if(dep.hash == 0) {
            return true;
        }

        // Missing means gone now — a change, since the build saw the file.
        // Unreadable cannot prove the disk unchanged and counts as changed
        // — conservative, retried by the rebuild's capture.
        if(files.check_version(files.intern_version(dep.path_id, dep.hash)) !=
           FileTable::Verdict::Fresh) {
            return true;
        }
    }
    return false;
}


std::shared_ptr<index::TUIndex> load_pch_envelope(llvm::StringRef path,
                                                  std::uint64_t expected_hash) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return nullptr;
    }
    if(expected_hash != 0 && llvm::xxh3_64bits((*buffer)->getBuffer()) != expected_hash) {
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
        state = load_pch_envelope(index_path, index_binding.hash);
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

void Workspace::build_module_map() {
    for(auto& [module_name, path_ids]: dep_graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = module_name.str();
        }
    }
}

void Workspace::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                              std::uint32_t exclude_path_id) const {
    for(auto& [pid, pcm_path]: pcm_paths) {
        if(pid == exclude_path_id)
            continue;
        auto mod_it = path_to_module.find(pid);
        if(mod_it != path_to_module.end()) {
            pcms[mod_it->second] = pcm_path;
        }
    }
}

}  // namespace clice
