module clice;

import :feature.diagnostics;
import :index.format;
import :project.command;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.graph.unit;
import :project.project;
import :support.anomaly;
import :support.failure_record;
import :support.json;
import :support.logging;
import :vfs.cache_store;
import :vfs.file_table;
import :worker.pool;
import :worker.protocol;

namespace clice {

namespace {

/// The precompiled records: a JSON document in the index database, the
/// persisted form of PCH/PCM validity metadata and self-containment
/// checks. Paths are persisted as spellings and re-interned at load —
/// runtime fids are not stable across sessions; deps persist as (spelling, content hash), so
/// the records reference no version ids and self-heal against any index
/// state.
struct SavedInput {
    /// index into the blob's paths table
    std::uint32_t path;

    std::uint64_t hash;

    bool missing;
};

struct SavedPCH {
    std::string key;

    /// CacheStore key in the "pch" namespace; absent in records written
    /// before pairs had names of their own.
    std::optional<std::string> blob;

    std::uint32_t bound;

    /// See PCHEntry::whole_file; absent from the records of a binary
    /// before it.
    KANNOTATE(&whole_file, presence = optional)
    bool whole_file = false;

    std::vector<SavedInput> deps;

    /// See PCHEntry::base; absent for none.
    std::optional<std::string> base;

    /// See PCHEntry::base_blob; absent for none, and in records written
    /// before it was recorded.
    std::optional<std::string> base_blob;
};

struct SavedPCM {
    std::string key;

    /// CacheStore key in the "pcm" namespace; absent in records written
    /// before PCMs had names of their own.
    std::optional<std::string> blob;

    std::uint32_t source_file;

    std::string module_name;

    std::vector<SavedInput> deps;
};

struct SavedSelfContainment {
    /// index into the blob's paths table
    std::uint32_t header;

    /// The command the header borrows (CommandTable::command_hash).
    std::uint64_t command;

    bool self_contained;

    /// The header's text the self-containment check compiled.
    std::uint64_t text;

    /// The header's disk content when the check ran.
    std::uint64_t disk;

    std::vector<SavedInput> deps;
};

struct Records {
    std::vector<std::string> paths;

    /// index_format_version the .pch.idx indexes were written with (one
    /// binary writes them all). A mismatch drops every PCH entry at load so
    /// the pairs rebuild immediately, instead of the mismatch showing up
    /// lazily on the first overlay query — which cannot trigger a rebuild.
    std::uint32_t pch_index_format = 0;

    std::vector<SavedPCH> pch;

    std::vector<SavedPCM> pcm;

    /// Absent from the records a binary older than the self-containment
    /// checks wrote.
    KANNOTATE(&trials, presence = optional)
    std::vector<SavedSelfContainment> trials;

    /// The missing_context_scoring the trials were judged under; absent
    /// from the records of a binary before it.
    KANNOTATE(&trials_scoring, presence = optional)
    std::uint32_t trials_scoring = 0;
};

}  // namespace

std::string cache_key(llvm::ArrayRef<llvm::StringRef> parts) {
    std::string input;
    for(auto part: parts) {
        input += std::format("{}:", part.size());
        input += part;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(input));
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

std::string new_blob(llvm::StringRef key) {
    std::random_device random;
    return std::format("{}-{:08x}{:08x}", key, random(), random());
}

FailureRecord failed_build(llvm::StringRef death, std::string cause) {
    FailureRecord record;
    record.fail({}, death, std::move(cause), FailureRecord::Clock::now());
    return record;
}

Inputs Project::read_inputs(const worker::PrecompiledBuildResult& built, const Inputs& before) {
    auto inputs = intern_inputs(file_table, built.deps, built.started_at);
    merge_inputs(inputs, {&before});
    return inputs;
}

void Project::merge_build_index(Fid unit,
                                IndexTransfer& transfer,
                                const worker::PrecompiledBuildResult& built,
                                const Inputs& before,
                                std::uint64_t command) {
    // One the build stopped compiling while it ran left the index with its
    // entries. A shared header's is the pass's.
    if(!is_unit(unit) && !shared_headers.files.contains(unit)) {
        return;
    }
    auto path = file_table.resolve(unit);
    auto tu_index = transfer.receive(built.tu_index_data, built.index_in_file);
    if(!tu_index) {
        LOG_WARN("The index the build of {} sent is lost: {}", path, tu_index.error());
        return;
    }
    // A build measures no peak: the unit's last compile's stands.
    auto manifest = index.manifest(unit);
    if(!merge_index(unit, *tu_index, before, command, manifest ? manifest->peak : 0)) {
        LOG_WARN("The index the build of {} sent did not merge", path);
    }
}

template <typename Params>
static async::task<std::expected<worker::PrecompiledBuildResult, Result>>
    send_build(Project& project,
               Attempt& attempt,
               const Params& params,
               llvm::StringRef label,
               Inputs known,
               const Inputs& before,
               llvm::function_ref<void(FailureRecord, Inputs)> fail) {
    constexpr bool pch = std::same_as<Params, worker::BuildPCHParams>;
    constexpr llvm::StringRef what = pch ? "PCH" : "PCM";
    auto priority = attempt.foreground() ? worker::Priority::High : worker::Priority::Low;
    auto result = co_await deliver(
        *project.pool,
        false,
        [&] { return project.pool->send_stateless(params, priority, attempt.token()); },
        [&](const ipc::Error& error) {
            fail(failed_build(worker::death_of(error), error.message), std::move(known));
        });
    if(result.has_value() && result.value().success) {
        co_return std::move(result.value());
    }
    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("{} build preempted for {}, will retry", what, label);
        co_return std::unexpected(Result::Stale);
    }
    // A build is expected to fail on the user's errors, or for an operational
    // reason (memory-pressure preemption, crash/restart window).
    auto expected = result.has_value() ? result.value().has_user_errors
                                       : worker::is_operational_error(result.error());
    auto& message = result.has_value() ? result.value().error : result.error().message;
    if(expected) {
        LOG_WARN("{} build failed for {}: {}", what, label, message);
        if(result.has_value()) {
            fail(failed_build({}, result.value().first_error),
                 project.read_inputs(result.value(), before));
        }
    } else if constexpr(pch) {
        LOG_ANOMALY(PCHBuildFail, "PCH build failed for {}: {}", label, message);
    } else {
        LOG_ANOMALY(PCMBuildFail, "PCM build failed for {}: {}", label, message);
    }
    co_return std::unexpected(Result::Failed);
}

async::task<std::expected<worker::PrecompiledBuildResult, Result>>
    Project::send_precompiled(Attempt& attempt,
                              const worker::BuildPCHParams& params,
                              llvm::StringRef label,
                              Inputs known,
                              const Inputs& before,
                              llvm::function_ref<void(FailureRecord, Inputs)> fail) {
    return send_build(*this, attempt, params, label, std::move(known), before, fail);
}

async::task<std::expected<worker::PrecompiledBuildResult, Result>>
    Project::send_precompiled(Attempt& attempt,
                              const worker::BuildPCMParams& params,
                              llvm::StringRef label,
                              Inputs known,
                              const Inputs& before,
                              llvm::function_ref<void(FailureRecord, Inputs)> fail) {
    return send_build(*this, attempt, params, label, std::move(known), before, fail);
}

std::string Project::serialize_precompiled_records() {
    Records data;
    data.pch_index_format = index::index_format_version;
    data.trials_scoring = feature::missing_context_scoring;
    llvm::StringMap<std::uint32_t> index_map;

    auto intern = [&](Fid fid) -> std::uint32_t {
        auto path = index.portable(file_table.resolve(fid));
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
    auto dump_deps = [&](const Inputs& snap, std::vector<SavedInput>& out) {
        for(auto& dep: snap) {
            auto hash = dep.version.valid() ? file_table.version(dep.version).content_hash : 0;
            out.push_back({intern(dep.file), hash, dep.absent});
        }
    };

    for(auto& e: pchs) {
        auto& st = e.second;
        // A shared header's lives for its pass.
        if(st.path.empty() || st.shared)
            continue;
        SavedPCH entry;
        entry.key = e.getKey().str();
        entry.blob = st.blob;
        entry.bound = st.bound;
        entry.whole_file = st.whole_file;
        dump_deps(st.inputs, entry.deps);
        if(!st.base.empty()) {
            entry.base = st.base;
            entry.base_blob = st.base_blob;
        }
        data.pch.push_back(std::move(entry));
    }

    for(auto& [path_id, st]: pcms) {
        if(st.path.empty())
            continue;
        SavedPCM entry;
        entry.key = st.key;
        entry.blob = st.blob;
        entry.source_file = intern(path_id);
        entry.module_name = dep_graph.module_of(path_id).str();
        dump_deps(st.inputs, entry.deps);
        data.pcm.push_back(std::move(entry));
    }

    hosting.self_containment.for_each([&](const SelfContainmentKey& key, const auto& entry) {
        SavedSelfContainment saved{
            .header = intern(key.first),
            .command = key.second,
            .self_contained = entry.result.self_contained,
            .text = file_table.version(entry.result.text).content_hash,
            .disk = file_table.version(entry.result.disk).content_hash,
        };
        dump_deps(entry.inputs, saved.deps);
        data.trials.push_back(std::move(saved));
    });

    auto json = json::to_string(data, path_json);
    if(!json) {
        LOG_WARN("Failed to serialize the precompiled records");
        return {};
    }
    return std::move(*json);
}

void Project::load_precompiled_records(llvm::StringRef bytes) {
    if(bytes.empty() || !store) {
        return;
    }
    Records data;
    if(!json::from_string(bytes, data)) {
        LOG_WARN("Failed to parse the precompiled records");
        return;
    }

    auto file_of = [&](std::uint32_t idx) -> std::optional<Fid> {
        if(idx >= data.paths.size() || data.paths[idx].empty()) {
            return std::nullopt;
        }
        return file_table.intern(index.local(data.paths[idx]));
    };
    auto load_deps = [&](const std::vector<SavedInput>& dep_entries) -> Inputs {
        Inputs inputs;
        for(auto& dep: dep_entries) {
            auto dep_file = file_of(dep.path);
            if(!dep_file)
                continue;
            auto& input = inputs.emplace_back();
            input.file = *dep_file;
            input.absent = dep.missing;
            if(dep.hash != 0) {
                input.version = file_table.intern_version(input.file, dep.hash);
            }
        }
        return inputs;
    };

    bool pch_format_ok = data.pch_index_format == index::index_format_version;
    for(auto& entry: data.pch) {
        if(!pch_format_ok) {
            break;
        }
        if(!entry.blob) {
            store->invalidate("pch", entry.key);
            continue;
        }
        auto pch_path = store->lookup("pch", *entry.blob);
        if(!pch_path)
            continue;
        // A preamble's PCH without its pch.idx index is an incomplete pair
        // (crash between the two commits): drop it so the next compile
        // rebuilds both.
        std::optional<std::string> index_path;
        if(!entry.whole_file) {
            index_path = store->lookup_aux("pch", *entry.blob);
            if(!index_path) {
                store->invalidate("pch", *entry.blob);
                continue;
            }
        }

        auto& st = pchs[entry.key];
        st.blob = std::move(*entry.blob);
        st.path = *pch_path;
        st.bound = entry.bound;
        st.whole_file = entry.whole_file;
        st.inputs = load_deps(entry.deps);
        st.base = entry.base.value_or("");
        st.base_blob = entry.base_blob.value_or("");
        st.index_path = index_path.value_or("");
    }

    for(auto& entry: data.pcm) {
        if(!entry.blob) {
            store->invalidate("pcm", entry.key);
            continue;
        }
        auto pcm_path = store->lookup("pcm", *entry.blob);
        // A dep-less entry is an unvalidatable snapshot that would blindly
        // serve a stale PCM (the key embeds no content). Drop it and let
        // the module rebuild once.
        if(!pcm_path || entry.deps.empty())
            continue;
        auto source = file_of(entry.source_file);
        if(!source)
            continue;
        auto& st = pcms[*source];
        st.path = *pcm_path;
        st.key = entry.key;
        st.blob = std::move(*entry.blob);
        st.inputs = load_deps(entry.deps);
    }

    if(data.trials_scoring != feature::missing_context_scoring) {
        data.trials.clear();
    }
    for(auto& saved: data.trials) {
        auto header = file_of(saved.header);
        if(!header) {
            continue;
        }
        hosting.self_containment.commit({*header, saved.command},
                                        {.self_contained = saved.self_contained,
                                         .text = file_table.intern_version(*header, saved.text),
                                         .disk = file_table.intern_version(*header, saved.disk)},
                                        load_deps(saved.deps));
    }

    LOG_INFO("Loaded precompiled records: {} PCH entries, {} PCM entries",
             pchs.size(),
             pcms.size());
}

}  // namespace clice
