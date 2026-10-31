module clice;

import :vfs.file_table;

namespace clice {

/// `path` with its `..` segments resolved as text.
static Spelling fold(const Spelling& path) {
    llvm::SmallString<256> text(path);
    path::normalize(text);
    return Spelling::absolute(text);
}

Fid FileTable::intern(const Spelling& path) {
    // FIXME: a spelling keeps the identity it first resolved to: a directory
    // that later appears as a symlink, or a case-only rename, leaves the file a
    // second fid until restart.
    if(auto it = ids.find(path); it != ids.end()) {
        return it->second;
    }
    auto fid = intern(CanonicalPath(path));
    ids.try_emplace(path, fid);
    return fid;
}

Fid FileTable::intern_spelled(const Spelling& path) {
    auto fid = intern(path);
    spell_as(fid, path);
    return fid;
}

Fid FileTable::intern(CanonicalRef identity) {
    auto [it, inserted] =
        ids.try_emplace(identity, Fid{static_cast<std::uint32_t>(spellings.size())});
    if(inserted) {
        // Null-terminated, so that resolve().data() is safe to use as
        // const char* (e.g. in MemoryBuffer::getFile which calls strlen).
        spellings.push_back(llvm::StringSaver(allocator).save(llvm::StringRef(identity)));
    }
    return it->second;
}

std::optional<Fid> FileTable::find(const Spelling& path) const {
    auto it = ids.find(path);
    if(it == ids.end()) {
        it = ids.find(CanonicalPath(path));
    }
    if(it == ids.end()) {
        return std::nullopt;
    }
    return it->second;
}

void FileTable::spell_as(Fid fid, const Spelling& path) {
    if(llvm::StringRef(path) != llvm::StringRef(resolve(fid))) {
        spelled.try_emplace(fid, path.str());
    }
}

Spelling FileTable::spelling(Fid fid) const {
    if(auto it = spelled.find(fid); it != spelled.end()) {
        return Spelling::absolute(it->second);
    }
    return Spelling(resolve(fid));
}

std::string FileTable::display_identity(llvm::StringRef identity) {
    return display(intern(Spelling::absolute(identity)));
}

std::string FileTable::display(Fid fid) const {
    if(auto it = shown.find(fid); it != shown.end()) {
        return it->second;
    }
    return display(resolve(fid));
}

std::string FileTable::display(CanonicalRef identity) const {
    for(auto& [real, root]: spelled_roots) {
        if(path::is_within(identity, real)) {
            auto rest = llvm::StringRef(identity).drop_front(real.size()).ltrim('/');
            return Spelling(rest, root).str();
        }
    }
    return identity.str();
}

void FileTable::spell_root(const Spelling& root) {
    CanonicalPath real(root);
    auto folded = fold(root);
    if(folded.str() != real.str() && CanonicalPath(folded) == real) {
        spelled_roots.emplace_back(std::move(real), std::move(folded));
    }
}

void FileTable::unspell_root(const Spelling& root) {
    auto folded = fold(root);
    spelled_roots.erase(
        ranges::remove_if(spelled_roots, [&](auto& entry) { return entry.second == folded; })
            .begin(),
        spelled_roots.end());
}

VersionID FileTable::intern_version(Fid fid, std::uint64_t content_hash) {
    auto [it, inserted] =
        version_ids.try_emplace({fid, content_hash},
                                VersionID{static_cast<std::uint32_t>(versions.size())});
    if(inserted) {
        versions.push_back(FileVersion{.fid = fid, .content_hash = content_hash});
    }
    return it->second;
}

vfs::DiskState::CheckResult FileTable::check_version(VersionID vid) {
    auto& version = this->version(vid);
    return disk.check(version.fid, version.content_hash);
}

bool inputs_hold(FileTable& files, const Inputs& snap) {
    auto changed = [&](const Input& dep) {
        // Gone at build time: reappearing is the change; still-missing
        // stays unchanged (see intern_inputs).
        if(dep.absent) {
            return files.disk.present(dep.file);
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
    return ranges::count_if(snap, changed) == 0;
}

Inputs intern_inputs(FileTable& files,
                     llvm::ArrayRef<ReportedInput> deps,
                     std::int64_t started_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build".
    auto baseline_before_ns = vfs::stat_baseline_before_ns(started_at);

    Inputs snap;
    snap.reserve(deps.size());
    vfs::StatusBatch statuses;
    for(const auto& file: deps) {
        auto& dep = snap.emplace_back();
        dep.file = files.intern(Spelling::absolute(file.path));
        auto hash = file.hash;

        // A place a failed lookup looked: the build saw nothing there,
        // whatever is there by now. The file table watches it from here on;
        // a file there is a change.
        if(file.absent) {
            dep.absent = true;
            files.disk.current(dep.file);
            continue;
        }

        auto status = statuses.status(file.path);
        if(!status) {
            // A file the build read that is gone already: record the
            // absence, reappearing counts as a change. Still-missing
            // deliberately counts as unchanged — flagging it would rebuild
            // on every check without ever converging, while the product is
            // the last remaining truth for the file (and dependents'
            // recovery is the DiskRemoved cascade's job, not this
            // snapshot's).
            dep.absent = true;
            files.disk.saw_missing(dep.file);
            continue;
        }

        if(hash == 0) {
            // The worker could not hash the consumed bytes: when nothing
            // else names them, the dep stays version-less and reads as
            // changed until the rebuild's capture retries.
            hash = files.disk.unchanged_hash(dep.file, *status, baseline_before_ns);
            if(hash == 0) {
                continue;
            }
        } else {
            files.disk.consumed(dep.file, hash);
        }

        dep.version = files.intern_version(dep.file, hash);
    }
    return snap;
}

std::optional<Input> current_input(FileTable& files, Fid file) {
    if(auto seen = files.disk.current(file)) {
        return Input{.file = file, .version = files.intern_version(file, seen->hash)};
    }
    if(files.disk.seen_missing(file)) {
        return Input{.file = file, .absent = true};
    }
    return std::nullopt;
}

void merge_inputs(Inputs& into, llvm::ArrayRef<const Inputs*> parts) {
    auto key = [](const Input& dep) {
        return std::tuple(dep.file.raw, dep.version.raw, std::uint8_t(dep.absent));
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

}  // namespace clice
