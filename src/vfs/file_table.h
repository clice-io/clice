#pragma once

#include <cassert>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "support/filesystem.h"
#include "syntax/scan.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

/// One observation of a file's on-disk bytes: the xxh3 of the text a single
/// read returned (see without_bom), and the stat describing the bytes. Captured under
/// the pairing discipline (see read_file_observed) so the two halves are
/// same-source: `paired` says the pre/post fstats of the read agreed,
/// `reliable` additionally says the mtime lay outside the filesystem
/// mtime-granularity guard window — only then may the stat serve as a
/// fast-path baseline for skipping future reads. An unpaired or
/// unreliable observation still carries a true hash of the bytes read.
struct DiskObservation {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint64_t hash = 0;
    /// Filesystem identity of the inode the bytes were read from
    /// (fstat's UniqueID) — what binds a spelling to an entity.
    std::uint64_t uid_device = 0;
    std::uint64_t uid_file = 0;
    bool paired = false;
    bool reliable = false;
};

/// A completed observed read: the observation plus the text it hashed.
struct ObservedFile {
    DiskObservation obs;
    std::unique_ptr<llvm::MemoryBuffer> content;
};

/// A file id: the FileTable's compact handle for one interned path
/// spelling. A distinct type so fids, version ids and other integers
/// cannot mix silently. Default-constructed = invalid ("no file").
struct Fid {
    std::uint32_t raw = ~0u;

    constexpr bool valid() const {
        return raw != ~0u;
    }

    friend constexpr auto operator<=>(Fid, Fid) = default;
};

/// A version id: the FileTable's handle for one (file, content hash)
/// pair. Default-constructed = invalid ("no version").
struct VersionID {
    std::uint32_t raw = ~0u;

    constexpr bool valid() const {
        return raw != ~0u;
    }

    friend constexpr auto operator<=>(VersionID, VersionID) = default;
};

}  // namespace clice

template <>
struct llvm::DenseMapInfo<clice::Fid> {
    static unsigned getHashValue(clice::Fid fid) {
        return DenseMapInfo<std::uint32_t>::getHashValue(fid.raw);
    }

    static bool isEqual(clice::Fid lhs, clice::Fid rhs) {
        return lhs == rhs;
    }
};

template <>
struct llvm::DenseMapInfo<clice::VersionID> {
    static unsigned getHashValue(clice::VersionID vid) {
        return DenseMapInfo<std::uint32_t>::getHashValue(vid.raw);
    }

    static bool isEqual(clice::VersionID lhs, clice::VersionID rhs) {
        return lhs == rhs;
    }
};

/// Ids appear directly in log messages and test-failure output; format
/// as the raw id.
template <>
struct std::formatter<clice::Fid> : std::formatter<std::uint32_t> {
    auto format(clice::Fid fid, auto& ctx) const {
        return std::formatter<std::uint32_t>::format(fid.raw, ctx);
    }
};

template <>
struct std::formatter<clice::VersionID> : std::formatter<std::uint32_t> {
    auto format(clice::VersionID vid, auto& ctx) const {
        return std::formatter<std::uint32_t>::format(vid.raw, ctx);
    }
};

namespace clice {

/// Read a file and hash its bytes under the pairing discipline: open a
/// handle, fstat it, read through it, fstat again. Equal fstats prove
/// the stat describes the bytes (an in-place write racing the read moves
/// the mtime between the two fstats; a rename-over does not affect the
/// open handle at all). A post-fstat mtime inside the guard window
/// (coarse-granularity filesystems) demotes the pair to unreliable: a
/// racing write can land within one mtime tick, so such a stat must not
/// suppress future reads. Returns nullopt when the file cannot be
/// opened or read. Safe to call from any thread.
std::optional<ObservedFile> read_file_observed(const char* path);

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
        if(llvm::StringRef(path) != llvm::StringRef(resolve(fid)) &&
           spelled.try_emplace(fid, save(path.str())).second) {
            root_displays.erase(fid);
        }
    }

    Spelling spelling(Fid fid) const {
        if(auto it = spelled.find(fid); it != spelled.end()) {
            return Spelling::absolute(it->second);
        }
        return Spelling(resolve(fid));
    }

    /// The path a user knows a file by: the one its open document was
    /// opened under, else the build's spelling of it, under the spelling of
    /// a root it lies in (a workspace opened through a symlink). Everything
    /// the user is shown — URIs, query output — names files this way;
    /// identity never does.
    llvm::StringRef display(Fid fid) const {
        if(auto it = shown.find(fid); it != shown.end()) {
            return it->second;
        }
        llvm::StringRef path = resolve(fid);
        if(auto it = spelled.find(fid); it != spelled.end()) {
            path = it->second;
        }
        for(auto& [real, spelled_root]: spelled_roots) {
            if(path::under(path, llvm::StringRef(real))) {
                auto [it, inserted] = root_displays.try_emplace(fid);
                if(inserted) {
                    it->second = save(spelled_root + path.drop_front(real.size()).str());
                }
                return it->second;
            }
        }
        return path;
    }

    /// An open document names its file this way until it closes.
    void show_as(Fid fid, llvm::StringRef spelling) {
        shown[fid] = save(spelling);
    }

    /// The spelling the file's open document was opened under, if one is.
    std::optional<llvm::StringRef> shown_as(Fid fid) const {
        auto it = shown.find(fid);
        return it != shown.end() ? std::optional(it->second) : std::nullopt;
    }

    void unshow(Fid fid) {
        shown.erase(fid);
    }

    /// Files under `root` show under this spelling of it.
    void spell_root(const Spelling& root) {
        CanonicalPath real(root);
        if(real.str() != root.str()) {
            spelled_roots.emplace_back(std::move(real), root.str());
            root_displays.clear();
        }
    }

    /// Stop showing files under a spelling spell_root recorded.
    void unspell_root(const Spelling& root) {
        llvm::erase_if(spelled_roots, [&](auto& entry) { return entry.second == root.str(); });
        root_displays.clear();
    }

    llvm::DenseMap<Fid, llvm::StringRef> shown;
    llvm::DenseMap<Fid, llvm::StringRef> spelled;
    llvm::SmallVector<std::pair<CanonicalPath, std::string>> spelled_roots;
    mutable llvm::DenseMap<Fid, llvm::StringRef> root_displays;
    mutable llvm::BumpPtrAllocator display_storage;

    llvm::StringRef save(llvm::StringRef text) const {
        auto* buf = display_storage.Allocate<char>(text.size());
        std::ranges::copy(text, buf);
        return llvm::StringRef(buf, text.size());
    }

    /// Entities: on-disk files merged by filesystem UniqueID, the way
    /// clang's FileManager merges FileEntries — hardlinked or symlinked
    /// spellings of one file share the content-derived facts below. A
    /// fid's binding to an entity is itself stat-verified: every
    /// observation carries the UniqueID its stat returned, and a mismatch
    /// rebinds (editors save via tmp+rename, so a spelling changes inode
    /// on every save). What was last seen on disk (`seen`) stays per fid:
    /// shared per entity, a save through one hardlink spelling would
    /// swallow the other spelling's change event.
    ///
    /// FIXME: UniqueID reliability on network filesystems is inherited
    /// from clang's known limitation — some report unstable or colliding
    /// ids, which here degrades to spurious rebinds (extra reads), never
    /// wrong hashes (the first read through a rebound fid re-earns trust).
    /// On Windows the ids are not file-stable everywhere (see
    /// fs::stable_file_ids), so entity merging is disabled there wholesale
    /// via entity_key: identity-based defenses are POSIX-only.
    llvm::DenseMap<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> entity_ids;

    struct EntityBinding {
        std::uint32_t entity = ~0u;

        /// A read through THIS fid confirmed the binding. Until then the
        /// entity's pair is withheld from the fid: a recycled inode can
        /// hand an unrelated new file an existing entity with an
        /// equal-looking stat, and inheriting its pair would serve the
        /// old file's hash for the new file's bytes.
        bool earned = false;
    };

    llvm::DenseMap<Fid, EntityBinding> bindings;

    /// Last reliable same-source {stat, hash} pair per entity: the shared
    /// baseline every consumer's staleness check draws from and repairs —
    /// computed once, shared by every spelling of the file.
    llvm::DenseMap<std::uint32_t, DiskObservation> disk_states;

    /// The entity-map key for an identity observed through a fid. Where
    /// file IDs are not file-stable (see fs::stable_file_ids), every
    /// spelling is its own entity and the observed id is ignored:
    /// hardlinks stay unmerged, and replace detection falls back to the
    /// stat pair — the pre-entity behavior.
    static std::pair<std::uint64_t, std::uint64_t> entity_key(Fid fid,
                                                              std::uint64_t uid_device,
                                                              std::uint64_t uid_file) {
        if constexpr(!fs::stable_file_ids) {
            return {~0ull, fid.raw};
        }
        return {uid_device, uid_file};
    }

    /// Re-verify (and if needed re-establish) the fid's entity binding
    /// against the identity a live stat just returned. Returns the
    /// binding; `earned` is false until a read through this fid confirms
    /// it (see EntityBinding). A fid binding to a brand-new entity has
    /// nothing to wrongly inherit, so it is born earned.
    EntityBinding& bind(Fid fid, std::uint64_t uid_device, std::uint64_t uid_file) {
        auto [it, fresh] = entity_ids.try_emplace(entity_key(fid, uid_device, uid_file),
                                                  static_cast<std::uint32_t>(entity_ids.size()));
        auto entity = it->second;
        auto& binding = bindings[fid];
        if(binding.entity != entity) {
            binding.entity = entity;
            binding.earned = fresh;
        }
        return binding;
    }

    /// The cached hash of exactly this (size, mtime) at exactly this
    /// filesystem identity, or nullopt when someone must read. Equality
    /// against the shared pair, never a watermark: the hash is "the hash
    /// of the bytes that had this stat", nothing else.
    std::optional<std::uint64_t> cached_hash(Fid fid,
                                             std::uint64_t size,
                                             std::int64_t mtime_ns,
                                             std::uint64_t uid_device,
                                             std::uint64_t uid_file) {
        auto& binding = bind(fid, uid_device, uid_file);
        if(!binding.earned) {
            return std::nullopt;
        }
        auto it = disk_states.find(binding.entity);
        if(it == disk_states.end()) {
            return std::nullopt;
        }
        auto& pair = it->second;
        if(pair.size != size || pair.mtime_ns != mtime_ns) {
            return std::nullopt;
        }
        saw(fid, pair.hash);
        return pair.hash;
    }

    /// What the disk held at the last look through each fid: the content
    /// hash, or nullopt when the file was missing. No entry before the
    /// first look. Every read, every stat the shared pair vouches for and
    /// every failed stat of a freshness check or sweep writes it — the one
    /// record of "what is on disk now", lagging the disk by at most the
    /// time since the last look.
    llvm::DenseMap<Fid, std::optional<std::uint64_t>> seen;

    /// Files whose seen content moved from one known state to another
    /// since the last take_changes(), in first-change order: the table is
    /// the single source of disk change events, whoever happened to look
    /// (the workspace sweep, a save, a rescan, a compile's staleness
    /// check). A first look is no change — nothing was derived from an
    /// unseen state.
    llvm::SmallVector<Fid> changes;
    llvm::DenseSet<Fid> changed;

    /// Invoked when `changes` goes from empty to non-empty; the owner
    /// schedules the drain. Unset (batch tools, tests) leaves the queue to
    /// whoever takes it.
    std::function<void()> on_change;

    /// The disk content as last seen through this fid, without I/O;
    /// nullopt before the first look and while the file is missing.
    std::optional<std::uint64_t> seen_hash(Fid fid) const {
        auto it = seen.find(fid);
        return it != seen.end() ? it->second : std::nullopt;
    }

    /// A look found the file missing.
    void saw_missing(Fid fid) {
        saw(fid, std::nullopt);
    }

    /// Whether the last look through this fid found the file missing.
    bool seen_missing(Fid fid) const {
        auto it = seen.find(fid);
        return it != seen.end() && !it->second;
    }

    /// Every fid the last look found missing: deleted files, and the places
    /// failed lookups looked — where a file appearing is a change.
    llvm::SmallVector<Fid> missing_files() const {
        llvm::SmallVector<Fid> result;
        for(auto& [fid, hash]: seen) {
            if(!hash) {
                result.push_back(fid);
            }
        }
        return result;
    }

    /// The changed files, in first-change order, emptying the queue.
    llvm::SmallVector<Fid> take_changes() {
        changed.clear();
        return std::exchange(changes, {});
    }

    /// Record a same-source read (the scan worker's, or one made through
    /// read()) as the entity's shared pair; the read also earns the fid
    /// its binding. Unpaired reads carry a true hash but no stat proof,
    /// so they never become the pair.
    void observe(Fid fid, const DiskObservation& obs) {
        auto& binding = bind(fid, obs.uid_device, obs.uid_file);
        binding.earned = true;
        saw(fid, obs.hash);
        if(obs.reliable) {
            disk_states[binding.entity] = obs;
        }
    }

    /// Read the file under the pairing discipline and refresh the shared
    /// pair. nullopt = unreadable right now (the pair is left untouched;
    /// what a failed read means is the caller's policy).
    std::optional<DiskObservation> read(Fid fid) {
        auto observed = read_file_observed(resolve(fid).data());
        if(!observed) {
            return std::nullopt;
        }
        observe(fid, observed->obs);
        return observed->obs;
    }

    /// Stat the file and produce a same-source observation of its
    /// current content. nullopt = missing or unreadable.
    std::optional<DiskObservation> current(Fid fid) {
        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(resolve(fid), status)) {
            saw_missing(fid);
            return std::nullopt;
        }
        return observe_for(fid, status);
    }

    /// The two-layer primitive: a same-source observation for a live
    /// stat the caller just took — the shared pair when it matches by
    /// equality (and the filesystem identity confirms the binding), else
    /// a real read (which repairs the pair for every later consumer; its
    /// observation may describe a newer stat than the caller's, which is
    /// then simply newer truth). nullopt = unreadable right now.
    std::optional<DiskObservation> observe_for(Fid fid, const llvm::sys::fs::file_status& status) {
        auto size = status.getSize();
        auto mtime_ns = fs::mtime_ns(status);
        auto uid = status.getUniqueID();
        if(auto hash = cached_hash(fid, size, mtime_ns, uid.getDevice(), uid.getFile())) {
            return DiskObservation{.size = size,
                                   .mtime_ns = mtime_ns,
                                   .hash = *hash,
                                   .uid_device = uid.getDevice(),
                                   .uid_file = uid.getFile(),
                                   .paired = true,
                                   .reliable = true};
        }
        return read(fid);
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

    /// How one wave's check of a version came out. Policy-free facts;
    /// what Missing or Unreadable *means* differs per consumer (see the
    /// policy table in the plan) and stays with the caller.
    enum class Verdict : std::uint8_t {
        /// The disk provably holds the version's bytes.
        Fresh,
        /// The disk holds different bytes.
        Stale,
        /// The file does not exist now.
        Missing,
        /// The file exists but cannot be read right now.
        Unreadable,
    };

    /// The lexical scan of a version's bytes: scan_quick is a pure
    /// function of the content, so the result is pinned by the version
    /// identity (fid, content hash) and every consumer at that version
    /// shares one lex — the startup scan feeds it, didSave rescans and
    /// CDB-reload rescans hit it when only the stat moved. Keyed by the
    /// identity pair rather than a version id so the scan (which runs
    /// before the persisted id space loads) never allocates ids. Raw
    /// results only — the module-name backfill below is configuration
    /// output and must not enter a content-keyed slot.
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

    /// A module declaration hidden behind preprocessor conditionals,
    /// resolved by a real preprocessor run under one compile
    /// configuration: keyed by (content hash, semantic hash of the
    /// rendered command) — the same bytes legitimately resolve differently
    /// under different flag sets, and dense config ids are CDB-local
    /// (multiple CDBs share this table).
    struct ModuleDecl {
        std::string name;
        bool is_interface_unit = false;
    };

    llvm::DenseMap<std::pair<std::uint64_t, std::uint64_t>, ModuleDecl> module_decls;

    /// Directory listings, validated by the directory's own mtime: POSIX
    /// and Windows bump it on entry creation and deletion, so one stat per
    /// operation proves a cached listing current — external generators
    /// dropping files into include directories produce no event, making
    /// the mtime the only anchor there is. mtime_ns == 0 means the listing
    /// was taken inside the mtime-granularity guard window (or the stat
    /// failed) and must not be trusted across operations; a listing
    /// re-earns trust at the next readdir. Deliberate residual: a forged
    /// (backdated) directory mtime defeats this — files have the content
    /// hash as a second anchor, a directory's only deeper truth is the
    /// readdir itself, and re-reading every use would mean not caching.
    struct DirListing {
        llvm::StringSet<> entries;
        std::int64_t mtime_ns = 0;
    };

    llvm::StringMap<DirListing> dir_listings;

    /// Wave-scoped verdict memo: one top-level check operation (a
    /// deps_changed chain, an index need_update batch) opens a Wave, and
    /// every version is settled at most once inside it.
    llvm::DenseMap<VersionID, Verdict> wave_verdicts;
    bool wave_open = false;

    /// RAII scope of one memo wave: verdicts live exactly as long as the
    /// guard, so a memo of one operation can never leak into the next.
    /// Waves do not nest, and a wave must not span a suspension point —
    /// a save landing mid-wave would leave memoized verdicts describing
    /// the old disk.
    class [[nodiscard]] Wave {
    public:
        explicit Wave(FileTable& table) : table(table) {
            assert(!table.wave_open && "waves do not nest");
            table.wave_open = true;
        }

        ~Wave() {
            table.wave_verdicts.clear();
            table.wave_open = false;
        }

        Wave(const Wave&) = delete;
        Wave& operator=(const Wave&) = delete;

    private:
        FileTable& table;
    };

    Wave wave() {
        return Wave(*this);
    }

    /// Whether the disk still holds a version's bytes: a live stat, the
    /// file's observation for it (observe_for: the shared pair, else a
    /// read), and the hash compared. Memoized within the current wave.
    Verdict check_version(VersionID vid) {
        assert(wave_open && "check_version outside a Wave");
        if(auto it = wave_verdicts.find(vid); it != wave_verdicts.end()) {
            return it->second;
        }
        auto verdict = check_version_uncached(vid);
        wave_verdicts.try_emplace(vid, verdict);
        return verdict;
    }

private:
    void saw(Fid fid, std::optional<std::uint64_t> hash) {
        auto [it, first] = seen.try_emplace(fid, hash);
        if(first || it->second == hash) {
            return;
        }
        it->second = hash;
        if(changed.insert(fid).second) {
            changes.push_back(fid);
            if(changes.size() == 1 && on_change) {
                on_change();
            }
        }
    }

    Verdict check_version_uncached(VersionID vid) {
        auto& version = this->version(vid);
        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(resolve(version.fid), status)) {
            saw_missing(version.fid);
            return Verdict::Missing;
        }
        // 0 is the consumed-hash sentinel for "the worker had no bytes to
        // hash": nothing to compare against, never fresh.
        if(version.content_hash == 0) {
            return Verdict::Stale;
        }
        auto obs = observe_for(version.fid, status);
        if(!obs) {
            return Verdict::Unreadable;
        }
        return obs->hash == version.content_hash ? Verdict::Fresh : Verdict::Stale;
    }
};

}  // namespace clice
