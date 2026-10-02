#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "syntax/scan.h"
#include "vfs/dir_cache.h"
#include "vfs/disk_state.h"
#include "vfs/file_system.h"
#include "vfs/ids.h"
#include "vfs/path.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

/// The master-side table of every file the workspace touches: a path is
/// interned once to a compact fid, and downstream code references files
/// by fid. A fid names a file's identity (CanonicalPath): symlinked and,
/// on Windows, case-variant, junction and subst spellings of one file
/// share it, while hardlinks stay distinct fids.
///
/// FIXME: paths are assumed to be valid UTF-8. POSIX filenames
/// are raw bytes; a non-UTF-8 path survives interning but breaks
/// downstream where it is embedded into JSON (worker IPC, the query
/// protocol) or percent-decoded by clients that interpret URIs as UTF-8.
///
/// FIXME: NVCC option files are read during CDB parsing but are not
/// among the load's inputs (CompilationDatabase::inputs) — editing one
/// changes commands without touching compile_commands.json, so nothing
/// notices until the CDB itself changes.
struct FileTable {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> spellings;
    llvm::StringMap<Fid> ids;

    /// The file a path names: every spelling of it — through symlinks,
    /// `.`/`..` segments, and on Windows case variants, junctions and
    /// subst drives — interns to the fid of its identity, which is also
    /// what resolve() gives back. The worker names the files its compiles
    /// read by the same identity, so both sides of the boundary name one
    /// file by one fid. A spelling stays bound to the file it first
    /// resolved to: one that must follow a retargeted symlink (a database
    /// path) is resolved by its caller.
    Fid intern(const Spelling& path) {
        if(auto it = ids.find(path.str()); it != ids.end()) {
            return it->second;
        }
        auto fid = intern(CanonicalPath(path));
        ids.try_emplace(path.str(), fid);
        return fid;
    }

    /// Intern a path the build reaches its file by, remembering how it
    /// spells the file (spell_as).
    Fid intern_spelled(const Spelling& path) {
        auto fid = intern(path);
        spell_as(fid, path);
        return fid;
    }

    /// An identity names its own file.
    Fid intern(CanonicalRef identity) {
        auto [it, inserted] =
            ids.try_emplace(identity, Fid{static_cast<std::uint32_t>(spellings.size())});
        if(inserted) {
            // Allocate with null terminator so that resolve().data() is safe
            // to use as const char* (e.g. in MemoryBuffer::getFile which calls strlen).
            const std::size_t n = identity.size();
            char* buf = allocator.Allocate<char>(n + 1);
            std::ranges::copy(llvm::StringRef(identity), buf);
            buf[n] = '\0';
            spellings.push_back(llvm::StringRef(buf, n));
        }
        return it->second;
    }

    CanonicalRef resolve(Fid fid) const {
        assert(fid.raw < spellings.size());
        return CanonicalRef(spellings[fid.raw]);
    }

    /// Look up a path without interning it.
    std::optional<Fid> find(const Spelling& path) const {
        auto it = ids.find(path.str());
        if(it == ids.end()) {
            it = ids.find(CanonicalPath(path));
        }
        if(it == ids.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// The path the build reaches a file by, when it differs from its
    /// identity: its database entry's spelling, or the directory an include
    /// lookup found it through; the first one recorded holds. A quoted
    /// include searches from this path's directory, as clang's does from
    /// the name it opened the includer under.
    void spell_as(Fid fid, const Spelling& path) {
        if(llvm::StringRef(path) != llvm::StringRef(resolve(fid))) {
            spelled.try_emplace(fid, path.str());
        }
    }

    Spelling spelling(Fid fid) const {
        if(auto it = spelled.find(fid); it != spelled.end()) {
            return Spelling::absolute(it->second);
        }
        return Spelling(resolve(fid));
    }

    /// The path a user knows a file by: the one its open document was
    /// opened under, else its identity under the workspace folder it lies
    /// in. Everything the user is shown — URIs, query output — names files
    /// this way; a lookup spelling (spelling()) never does.
    std::string display(Fid fid) const {
        if(auto it = shown.find(fid); it != shown.end()) {
            return it->second;
        }
        return display(resolve(fid));
    }

    /// A path by its identity, under the spelling of the workspace folder
    /// it lies in (a folder opened through a symlink) or as it is outside
    /// every folder.
    std::string display(CanonicalRef identity) const;

    /// An open document names its file this way until it closes.
    void show_as(Fid fid, llvm::StringRef spelling) {
        shown[fid] = spelling.str();
    }

    /// The spelling the file's open document was opened under, if one is.
    std::optional<llvm::StringRef> shown_as(Fid fid) const {
        auto it = shown.find(fid);
        return it != shown.end() ? std::optional<llvm::StringRef>(it->second) : std::nullopt;
    }

    void unshow(Fid fid) {
        shown.erase(fid);
    }

    /// Files under the workspace folder `root` show under this spelling of
    /// it, its `..` segments folded away while the folded path still names
    /// the same directory.
    void spell_root(const Spelling& root);

    /// Stop showing files under a spelling spell_root recorded.
    void unspell_root(const Spelling& root);

    llvm::DenseMap<Fid, std::string> shown;
    llvm::DenseMap<Fid, std::string> spelled;
    llvm::SmallVector<std::pair<CanonicalPath, Spelling>> spelled_roots;

    /// What the disk held at each file's last look, and the changes those
    /// looks saw.
    vfs::DiskState disk{spellings};

    std::optional<std::uint64_t> seen_hash(Fid fid) const {
        return disk.seen_hash(fid);
    }

    bool seen_missing(Fid fid) const {
        return disk.seen_missing(fid);
    }

    llvm::SmallVector<Fid> take_changes() {
        return disk.take_changes();
    }

    void saw_missing(Fid fid) {
        disk.saw_missing(fid);
    }

    void observe(Fid fid, const DiskObservation& obs) {
        disk.observe(fid, obs);
    }

    std::optional<DiskObservation> read(Fid fid) {
        return disk.read(fid);
    }

    std::optional<DiskObservation> current(Fid fid) {
        return disk.current(fid);
    }

    std::optional<DiskObservation> observe_for(Fid fid, const vfs::Status& status) {
        return disk.observe_for(fid, status);
    }

    std::optional<std::uint64_t> cached_hash(Fid fid, const vfs::Stamp& stamp) {
        return disk.cached_hash(fid, stamp);
    }

    /// A content version of a file: `content_hash` names the bytes (for
    /// build artifacts, the bytes the build consumed — reported by the
    /// worker, never replaced by a later disk read). Whether the disk still
    /// holds them is asked of the file's own observation, never recorded
    /// on the version.
    struct FileVersion {
        Fid fid;
        std::uint64_t content_hash = 0;
    };

    /// Version table, indexed by VersionID. The table is append-only:
    /// persistence garbage-collects unreferenced versions from the blob it
    /// writes, never from memory, so a version one consumer stops
    /// referencing can still anchor another consumer's staleness check.
    /// Persisted indexes name versions by ids of their own
    /// (index::ProjectIndex maps them), so these ids live one session.
    llvm::SmallVector<FileVersion> versions;
    llvm::DenseMap<std::pair<Fid, std::uint64_t>, VersionID> version_ids;

    const FileVersion& version(VersionID vid) const {
        assert(vid.raw < versions.size());
        return versions[vid.raw];
    }

    /// The version id for (fid, content hash), interning a new record on
    /// first sight.
    VersionID intern_version(Fid fid, std::uint64_t content_hash) {
        auto [it, inserted] =
            version_ids.try_emplace({fid, content_hash},
                                    VersionID{static_cast<std::uint32_t>(versions.size())});
        if(inserted) {
            versions.push_back(FileVersion{.fid = fid, .content_hash = content_hash});
        }
        return it->second;
    }

    /// The lexical scan of a version's bytes: scan_quick is a pure
    /// function of the content, so the result is pinned by the version
    /// identity (fid, content hash) and every consumer at that version
    /// shares one lex — the startup scan feeds it, didSave rescans and
    /// CDB-reload rescans hit it when only the stat moved. Keyed by the
    /// identity pair rather than a version id so the scan (which runs
    /// before the persisted id space loads) never allocates ids. Raw
    /// results only — a module name a preprocessor run resolves is
    /// configuration output and must not enter a content-keyed slot.
    llvm::DenseMap<std::pair<Fid, std::uint64_t>, ScanResult> scan_results;

    /// The scan of exactly these bytes, whose hash the caller proved to be
    /// `content_hash` (a paired read), computed on first sight.
    const ScanResult& scan_of(Fid fid, std::uint64_t content_hash, llvm::StringRef content) {
        auto [it, inserted] = scan_results.try_emplace({fid, content_hash});
        if(inserted) {
            it->second = scan_quick(content);
        }
        return it->second;
    }

    /// Directory listings kept across operations.
    vfs::DirCache dirs;

    vfs::DiskState::Wave wave() {
        return disk.wave();
    }

    /// Whether the disk still holds a version's bytes, looked at once per
    /// wave.
    vfs::DiskState::Verdict check_version(VersionID vid) {
        auto& version = this->version(vid);
        return disk.check(version.fid, version.content_hash);
    }

    /// Whether a place a build found empty holds a readable file now,
    /// looked at once per wave.
    bool present(Fid fid) {
        return disk.present(fid);
    }
};

}  // namespace clice
