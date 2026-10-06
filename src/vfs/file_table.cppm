module;

#include "modules/prelude.h"

module clice:vfs.file_table;

import :vfs.dir_cache;
import :vfs.disk_state;
import :vfs.file_system;
import :vfs.ids;
import :vfs.path;

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
    Fid intern(const Spelling& path);

    /// Intern a path the build reaches its file by, remembering how it
    /// spells the file (spell_as).
    Fid intern_spelled(const Spelling& path);

    /// An identity names its own file.
    Fid intern(CanonicalRef identity);

    CanonicalRef resolve(Fid fid) const {
        assert(fid.raw < spellings.size());
        return CanonicalRef(spellings[fid.raw]);
    }

    /// Look up a path without interning it.
    std::optional<Fid> find(const Spelling& path) const;

    /// The path the build reaches a file by, when it differs from its
    /// identity: its database entry's spelling, or the directory an include
    /// lookup found it through; the first one recorded holds. A quoted
    /// include searches from this path's directory, as clang's does from
    /// the name it opened the includer under.
    void spell_as(Fid fid, const Spelling& path);

    Spelling spelling(Fid fid) const;

    /// The path a user knows a file by: the one its open document was
    /// opened under, else its identity under the workspace folder it lies
    /// in. Everything the user is shown — URIs, query output — names files
    /// this way; a lookup spelling (spelling()) never does.
    std::string display(Fid fid) const;

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
    VersionID intern_version(Fid fid, std::uint64_t content_hash);

    /// Directory listings kept across operations.
    vfs::DirCache dirs;

    /// Whether the disk still holds a version's bytes, looked at once per
    /// turn.
    vfs::DiskState::Verdict check_version(VersionID vid);

    /// Whether a place a build found empty holds a readable file now,
    /// looked at once per turn.
    bool present(Fid fid) {
        return disk.present(fid);
    }
};

}  // namespace clice
