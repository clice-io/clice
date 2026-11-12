module clice;

import :index.project_index;

namespace clice::index {

ProjectIndex::ProjectIndex(FileTable& files) : files(files) {}

ProjectIndex::~ProjectIndex() = default;
ProjectIndex::ProjectIndex(ProjectIndex&&) noexcept = default;

std::uint32_t ProjectIndex::pending_shard_writes() const {
    auto stored = [](const auto& records) {
        return ranges::count_if(records,
                                [](const auto& entry) { return entry.second.has_value(); });
    };
    return static_cast<std::uint32_t>(
        stored(shard_records.changed) + stored(shard_records.written) +
        stored(instantiation_records.changed) + stored(instantiation_records.written));
}

bool ProjectIndex::has_unsaved_state() const {
    return !shard_records.changed.empty() || !instantiation_records.changed.empty() ||
           !manifest_records.changed.empty() || !symbol_records.changed.empty() ||
           !local_records.changed.empty() || !file_records.changed.empty() ||
           !dependent_records.changed.empty() || !path_records.changed.empty() ||
           !version_records.changed.empty() || !internals_changed.empty() || !compact.empty() ||
           header_changed;
}

}  // namespace clice::index
