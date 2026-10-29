module clice;

import :index.format;
import :project.artifact_records;
import :project.command_resolver;
import :project.project;
import :support.json;
import :support.logging;
import :vfs.cache_store;
import :vfs.file_table;

namespace clice {

namespace {

/// The artifact records: a JSON document in the index database, the
/// persisted form of PCH/PCM validity metadata and header-mode results
/// (the cache.json successor). Paths are
/// persisted as spellings and re-interned at load — runtime fids are not
/// stable across sessions; deps persist as (spelling, content hash), so
/// the records reference no version ids and self-heal against any index
/// state.
struct CacheDepEntry {
    /// index into the blob's paths table
    std::uint32_t path;

    std::uint64_t hash;

    bool missing;
};

struct CachePCHEntry {
    std::string key;

    /// CacheStore key in the "pch" namespace; absent in records written
    /// before pairs had names of their own.
    std::optional<std::string> blob;

    std::uint32_t bound;

    std::vector<CacheDepEntry> deps;

    /// See PCHState::base; absent for none.
    std::optional<std::string> base;
};

struct CachePCMEntry {
    std::string key;

    /// CacheStore key in the "pcm" namespace; absent in records written
    /// before PCMs had names of their own.
    std::optional<std::string> blob;

    std::uint32_t source_file;

    std::string module_name;

    std::vector<CacheDepEntry> deps;
};

struct ArtifactRecords {
    std::vector<std::string> paths;

    /// index_format_version the .pch.idx indexes were written with (one
    /// binary writes them all). A mismatch drops every PCH entry at load so
    /// the pairs rebuild immediately, instead of the mismatch showing up
    /// lazily on the first overlay query — which cannot trigger a rebuild.
    std::uint32_t pch_index_format = 0;

    std::vector<CachePCHEntry> pch;

    std::vector<CachePCMEntry> pcm;

    /// The headers that need a context, named for the modes older records
    /// stored beside each one, so those records still load.
    std::vector<ContextHeaderEntry> header_modes;
};

}  // namespace

std::string serialize_artifact_records(Project& project, CommandResolver& commands) {
    ArtifactRecords data;
    data.pch_index_format = index::index_format_version;
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

    // Deps persist as (spelling, hash) pairs so they self-heal against any
    // index state. A version-less dep (missing or unhashable at capture)
    // writes hash 0 and reloads as one.
    auto dump_deps = [&](const DepsSnapshot& snap, std::vector<CacheDepEntry>& out) {
        for(auto& dep: snap) {
            auto hash =
                dep.version.valid() ? project.file_table.version(dep.version).content_hash : 0;
            out.push_back({intern(dep.path_id), hash, dep.missing});
        }
    };

    for(auto& e: project.pch_cache) {
        auto& st = e.second;
        if(st.path.empty())
            continue;
        CachePCHEntry entry;
        entry.key = e.getKey().str();
        entry.blob = st.blob;
        entry.bound = st.bound;
        dump_deps(st.deps, entry.deps);
        if(!st.base.empty()) {
            entry.base = st.base;
        }
        data.pch.push_back(std::move(entry));
    }

    for(auto& [path_id, st]: project.pcm_builds) {
        if(st.path.empty())
            continue;
        CachePCMEntry entry;
        entry.key = st.key;
        entry.blob = st.blob;
        entry.source_file = intern(path_id);
        entry.module_name = project.dep_graph.module_of(path_id).str();
        dump_deps(st.deps, entry.deps);
        data.pcm.push_back(std::move(entry));
    }

    commands.dump_context_headers(data.header_modes, intern);

    auto json = json::to_string<PathJsonConfig>(data);
    if(!json) {
        LOG_WARN("Failed to serialize the artifact records");
        return {};
    }
    return std::move(*json);
}

void load_artifact_records(Project& project, CommandResolver& commands, llvm::StringRef bytes) {
    if(bytes.empty() || !project.store) {
        return;
    }
    ArtifactRecords data;
    if(!json::from_string(bytes, data)) {
        LOG_WARN("Failed to parse the artifact records");
        return;
    }

    auto file_of = [&](std::uint32_t idx) -> std::optional<Fid> {
        if(idx >= data.paths.size() || data.paths[idx].empty()) {
            return std::nullopt;
        }
        return project.file_table.intern(project.index.local(data.paths[idx]));
    };
    auto load_deps = [&](const std::vector<CacheDepEntry>& dep_entries) -> DepsSnapshot {
        DepsSnapshot deps;
        for(auto& dep: dep_entries) {
            auto dep_file = file_of(dep.path);
            if(!dep_file)
                continue;
            auto& state = deps.emplace_back();
            state.path_id = *dep_file;
            state.missing = dep.missing;
            if(dep.hash != 0) {
                state.version = project.file_table.intern_version(state.path_id, dep.hash);
            }
        }
        return deps;
    };

    bool pch_format_ok = data.pch_index_format == index::index_format_version;
    for(auto& entry: data.pch) {
        if(!pch_format_ok) {
            break;
        }
        if(!entry.blob) {
            project.store->invalidate("pch", entry.key);
            continue;
        }
        auto pch_path = project.store->lookup("pch", *entry.blob);
        if(!pch_path)
            continue;
        // A PCH without its pch.idx index is an incomplete pair
        // (crash between the two commits): drop it so the next compile
        // rebuilds both.
        auto index_path = project.store->lookup_aux("pch", *entry.blob);
        if(!index_path) {
            project.store->invalidate("pch", *entry.blob);
            continue;
        }

        auto& st = project.pch_cache[entry.key];
        st.blob = std::move(*entry.blob);
        st.path = *pch_path;
        st.bound = entry.bound;
        st.deps = load_deps(entry.deps);
        st.base = entry.base.value_or("");
        st.index_path = *index_path;
    }

    for(auto& entry: data.pcm) {
        if(!entry.blob) {
            project.store->invalidate("pcm", entry.key);
            continue;
        }
        auto pcm_path = project.store->lookup("pcm", *entry.blob);
        // A dep-less entry is an unvalidatable snapshot that would blindly
        // serve a stale PCM (the key embeds no content). Drop it and let
        // the module rebuild once.
        if(!pcm_path || entry.deps.empty())
            continue;
        auto source = file_of(entry.source_file);
        if(!source)
            continue;
        auto& st = project.pcm_builds[*source];
        st.path = *pcm_path;
        st.key = entry.key;
        st.blob = std::move(*entry.blob);
        st.deps = load_deps(entry.deps);
    }

    commands.load_context_headers(data.header_modes, file_of);

    LOG_INFO("Loaded artifact metadata: {} PCH entries, {} PCM entries",
             project.pch_cache.size(),
             project.pcm_builds.size());
}

}  // namespace clice
