module clice;

import :command.file_kind;
import :project.command;
import :project.project;
import :vfs.disk_state;

namespace clice {

std::optional<bool> Project::self_contained(const SelfContainmentKey& key, bool open) {
    auto& files = file_table;
    // An X-macro list and the like compile only inside their includer.
    if(is_context_header_path(files.resolve(key.first))) {
        return false;
    }
    auto* entry = hosting.self_containment.lookup(files, key);
    if(!entry) {
        return std::nullopt;
    }
    // A result the other side does not take is kept for it. The editor's
    // resolutions look at the disk through what the file table saw last, as
    // they read the buffer, not the file.
    if(open) {
        auto seen = files.disk.seen_hash(key.first);
        if(!seen || *seen != files.version(entry->result.disk).content_hash) {
            return std::nullopt;
        }
    } else if(files.check_version(entry->result.text) != vfs::DiskState::CheckResult::Fresh) {
        return std::nullopt;
    }
    return entry->result.self_contained;
}

void Project::commit_self_containment(const SelfContainmentKey& key,
                                      bool self_contained,
                                      VersionID text,
                                      Inputs inputs) {
    auto disk = current_input(file_table, key.first);
    hosting.self_containment.commit(
        key,
        {
            .self_contained = self_contained,
            .text = text,
            // A header gone from disk holds no result.
            .disk = disk && disk->version.valid() ? disk->version : text,
        },
        std::move(inputs));
    mark_precompiled_dirty();
}

bool Project::header_saved(const SelfContainmentKey& key, VersionID text, bool leans) {
    auto* entry = hosting.self_containment.find(key);
    if(!entry) {
        return false;
    }
    mark_precompiled_dirty();
    if(entry->result.self_contained && leans) {
        hosting.self_containment.forget(key);
        return true;
    }
    entry->result.text = text;
    entry->result.disk = text;
    return false;
}

}  // namespace clice
