#pragma once

#include <cassert>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>

#include "support/filesystem.h"
#include "syntax/scan.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

/// One observation of a file's on-disk bytes: the xxh3 of the bytes a
/// single read returned, and the stat describing them. Captured under
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

/// A completed observed read: the observation plus the bytes it hashed.
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
    static clice::Fid getEmptyKey() {
        return {DenseMapInfo<std::uint32_t>::getEmptyKey()};
    }

    static clice::Fid getTombstoneKey() {
        return {DenseMapInfo<std::uint32_t>::getTombstoneKey()};
    }

    static unsigned getHashValue(clice::Fid fid) {
        return DenseMapInfo<std::uint32_t>::getHashValue(fid.raw);
    }

    static bool isEqual(clice::Fid lhs, clice::Fid rhs) {
        return lhs == rhs;
    }
};

template <>
struct llvm::DenseMapInfo<clice::VersionID> {
    static clice::VersionID getEmptyKey() {
        return {DenseMapInfo<std::uint32_t>::getEmptyKey()};
    }

    static clice::VersionID getTombstoneKey() {
        return {DenseMapInfo<std::uint32_t>::getTombstoneKey()};
    }

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

/// The master-side table of every file the workspace touches: a path
/// spelling is interned once to a compact fid, and downstream code
/// references files by fid. A fid names a spelling, not an on-disk
/// file — case variants or links to one file are distinct fids.
///
/// Paths are opaque byte strings interned in the canonical spelling of
/// path::canonical, so on Windows the URI form VS Code sends
/// ("file:///f%3A/...") and the "F:/..." form the CDB and clang report
/// intern to one ID — without that, every CDB lookup missed and compiles
/// fell back to guessed commands. POSIX paths are never rewritten.
///
/// FIXME: non-drive components keep their case, so case-variant
/// spellings of one file on a case-insensitive filesystem can still
/// intern to different IDs.
///
/// FIXME: paths are assumed to be valid UTF-8. POSIX filenames
/// are raw bytes; a non-UTF-8 path survives interning but breaks
/// downstream where it is embedded into JSON (worker IPC, the agentic
/// protocol) or percent-decoded by clients that interpret URIs as UTF-8.
///
/// FIXME: @rsp and NVCC option files are read during CDB parsing but
/// never tracked here — editing one changes commands without touching
/// compile_commands.json, so nothing notices until the CDB itself
/// changes. Folding their paths into the CDB stamp is a follow-up.
struct FileTable {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> spellings;
    llvm::StringMap<Fid> ids;

    Fid intern(llvm::StringRef path) {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);

        auto [it, inserted] =
            ids.try_emplace(path, Fid{static_cast<std::uint32_t>(spellings.size())});
        if(inserted) {
            // Allocate with null terminator so that resolve().data() is safe
            // to use as const char* (e.g. in MemoryBuffer::getFile which calls strlen).
            const std::size_t n = path.size();
            char* buf = allocator.Allocate<char>(n + 1);
            std::ranges::copy(path, buf);
            buf[n] = '\0';
            spellings.push_back(llvm::StringRef(buf, n));
        }
        return it->second;
    }

    llvm::StringRef resolve(Fid fid) const {
        assert(fid.raw < spellings.size());
        return spellings[fid.raw];
    }

    /// Look up a path without interning it, applying the same
    /// normalization as intern().
    std::optional<Fid> find(llvm::StringRef path) const {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);
        auto it = ids.find(path);
        if(it == ids.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Entities: on-disk files merged by filesystem UniqueID, the way
    /// clang's FileManager merges FileEntries — hardlinked or symlinked
    /// spellings of one file share the content-derived facts below. A
    /// fid's binding to an entity is itself stat-verified: every
    /// observation carries the UniqueID its stat returned, and a mismatch
    /// rebinds (editors save via tmp+rename, so a spelling changes inode
    /// on every save). Consumer-specific observation state (what a
    /// consumer has *seen*, e.g. the tracker's last-reported baseline)
    /// stays with the consumer and per fid — shared, a save through one
    /// hardlink spelling would swallow the other spelling's change event.
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
        return pair.hash;
    }

    /// Record a same-source read (the scan worker's, or one made through
    /// read()) as the entity's shared pair; the read also earns the fid
    /// its binding. Unpaired reads carry a true hash but no stat proof,
    /// so they never become the pair.
    void observe(Fid fid, const DiskObservation& obs) {
        auto& binding = bind(fid, obs.uid_device, obs.uid_file);
        binding.earned = true;
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
    /// worker, never replaced by a later disk read), and the stat is the
    /// shared fast path proving the disk still holds them. Recorded only
    /// when the file provably did not change since before the consuming
    /// build started; mtime_ns == 0 means "no fast path" and a check
    /// falls through to the hash comparison, which repairs the fast path
    /// in place — once, for every consumer of the version.
    struct FileVersion {
        Fid fid;
        std::uint64_t content_hash = 0;
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
    };

    /// Version table, indexed by VersionID. Ids are monotonic and never
    /// reused — persisted index manifests stay resolvable against any
    /// later table (or are detected as stale). Within a session the table
    /// is append-only: persistence garbage-collects unreferenced versions
    /// from the blob it writes, never from memory, so a version one
    /// consumer stops referencing can still anchor another consumer's
    /// staleness check. Adopting a persisted table (load_global) leaves
    /// the garbage-collected ids as holes — records with an invalid fid
    /// that nothing references; knows_version tells them apart.
    llvm::SmallVector<FileVersion> versions;
    llvm::DenseMap<std::pair<Fid, std::uint64_t>, VersionID> version_ids;

    /// Whether the id names a live version (in range and not a hole left
    /// by adopting a garbage-collected persisted table).
    bool knows_version(VersionID vid) const {
        return vid.raw < versions.size() && versions[vid.raw].fid.valid();
    }

    /// Bumped whenever a version's stat fast path is written (stamped at
    /// capture or repaired by a check) or revoked (force_revalidate).
    /// Persistence compares it around an operation to learn whether the
    /// table changed under it.
    std::uint64_t stamp_generation = 0;

    /// Bumped only when force_revalidate revokes stamps, and persisted in
    /// both metadata blobs that carry them (the global blob's version
    /// table, the artifacts blob's dep records). The blobs commit
    /// non-atomically, so a crash can land a global recording a revocation
    /// next to an artifacts blob that predates it — whose stamps
    /// adopt_stamp would then restore into the revoked holes. Adoption is
    /// gated on the artifacts blob being at least as revocation-current as
    /// the loaded global.
    std::uint64_t revocation_generation = 0;

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

    /// Give a version its stat fast path, but only corroborated: the shared
    /// pair must prove the bytes at exactly this stat hash to the version's
    /// content hash. A caller's own proof (e.g. "mtime predates the build")
    /// is not enough — a same-stat rewrite forging the mtime would stamp a
    /// stat describing bytes the consumer never saw, and the equality fast
    /// path would then judge them fresh forever. An already-stamped version
    /// keeps its stamp (it was earned the same way; concurrent captures of
    /// one version must not regress each other).
    void try_stamp(VersionID vid,
                   std::uint64_t size,
                   std::int64_t mtime_ns,
                   std::uint64_t uid_device,
                   std::uint64_t uid_file) {
        assert(vid.raw < versions.size());
        auto& version = versions[vid.raw];
        if(version.mtime_ns != 0 || version.content_hash == 0) {
            return;
        }
        // Corroborate through the fid's earned binding; an unverified or
        // missing binding simply declines the stamp and the first check
        // earns it by reading.
        auto binding = bindings.find(version.fid);
        if(binding == bindings.end() || !binding->second.earned) {
            return;
        }
        // The live stat's identity must be the earned binding's: a
        // same-stat replace (new inode, forged size and mtime) would
        // otherwise corroborate through the replaced file's pair.
        auto entity = entity_ids.find(entity_key(version.fid, uid_device, uid_file));
        if(entity == entity_ids.end() || entity->second != binding->second.entity) {
            return;
        }
        auto pair = disk_states.find(binding->second.entity);
        if(pair == disk_states.end() || pair->second.size != size ||
           pair->second.mtime_ns != mtime_ns || pair->second.hash != version.content_hash) {
            return;
        }
        version.size = size;
        version.mtime_ns = mtime_ns;
        stamp_generation += 1;
    }

    /// Adopt a stamp persisted by an earlier session — it was earned under
    /// try_stamp's corroboration discipline back then, which is what makes
    /// it trustworthy without a live pair now. Only fills a hole: a stamp
    /// earned this session describes the same bytes at least as recently.
    void adopt_stamp(VersionID vid, std::uint64_t size, std::int64_t mtime_ns) {
        assert(vid.raw < versions.size());
        auto& version = versions[vid.raw];
        if(version.mtime_ns == 0 && version.content_hash != 0 && mtime_ns != 0) {
            version.size = size;
            version.mtime_ns = mtime_ns;
        }
    }

    /// A save embedded this file's content into artifacts that will not be
    /// re-read from disk (synthesized preambles): drop every trust anchor
    /// so the next check of any of its versions performs a real read — the
    /// stat fast paths, the shared pair a check would consult instead of
    /// reading, and verdicts already memoized in the current wave, which
    /// would bypass the forced point entirely.
    void force_revalidate(Fid fid) {
        // Entity-level: dropping only fid-scoped anchors would leave the
        // pair — and the version stamps of a hardlinked spelling of the
        // same file — vouching for bytes this call says to re-read.
        auto entity = ~0u;
        if(auto binding = bindings.find(fid); binding != bindings.end()) {
            entity = binding->second.entity;
            disk_states.erase(entity);
        }
        bool revoked = false;
        for(std::uint32_t i = 0; i < versions.size(); i += 1) {
            auto& version = versions[i];
            if(!version.fid.valid()) {
                continue;
            }
            bool same_file = version.fid == fid;
            if(!same_file && entity != ~0u) {
                auto alias = bindings.find(version.fid);
                same_file = alias != bindings.end() && alias->second.entity == entity;
            }
            if(same_file) {
                revoked = revoked || version.mtime_ns != 0;
                version.size = 0;
                version.mtime_ns = 0;
                wave_verdicts.erase(VersionID{i});
            }
        }
        // Revocation is stamp movement like any other: persisted stamps
        // (the global blob, artifact dep records) must not outlive it, or
        // the next session re-adopts trust this call just dropped.
        if(revoked) {
            stamp_generation += 1;
            revocation_generation += 1;
        }
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
    /// every version is settled at most once inside it. Within a wave, a
    /// version with no fast path is validated by a real read exactly once;
    /// the repair the read performs is what later waves' fast paths are
    /// made of.
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

    /// The unified two-layer staleness check: stat equality against the
    /// version's shared fast path, else a read through the disk-state
    /// compartment (feeding both compartments), comparing the bytes'
    /// hash against the version's and repairing the fast path on a
    /// match. Memoized within the current wave.
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
    /// Whether a stat-equality fast path may stand for this fid: once the
    /// session has learned the file's identity (an earned binding), the
    /// live stat must still carry it — a rename-over with a forged equal
    /// stat changes the UniqueID and must fall through to a read. A fid
    /// with no earned binding keeps cross-session trust: adopted stamps
    /// serve the cold start before any read has happened.
    bool stamp_identity_holds(Fid fid, const llvm::sys::fs::file_status& status) const {
        auto binding = bindings.find(fid);
        if(binding == bindings.end() || !binding->second.earned) {
            return true;
        }
        auto uid = status.getUniqueID();
        auto entity = entity_ids.find(entity_key(fid, uid.getDevice(), uid.getFile()));
        return entity != entity_ids.end() && entity->second == binding->second.entity;
    }

    Verdict check_version_uncached(VersionID vid) {
        assert(vid.raw < versions.size());
        auto& version = versions[vid.raw];

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(resolve(version.fid), status)) {
            return Verdict::Missing;
        }
        auto size = status.getSize();
        auto mtime_ns = fs::mtime_ns(status);
        if(version.mtime_ns != 0 && version.size == size && version.mtime_ns == mtime_ns &&
           stamp_identity_holds(version.fid, status)) {
            return Verdict::Fresh;
        }

        // No trusted hash to compare against: never fresh (0 is the
        // consumed-hash sentinel for "the worker had no bytes to hash").
        if(version.content_hash == 0) {
            return Verdict::Stale;
        }

        auto obs = observe_for(version.fid, status);
        if(!obs) {
            return Verdict::Unreadable;
        }
        if(obs->hash != version.content_hash) {
            return Verdict::Stale;
        }
        // Touched but not modified — repair the fast path so the next
        // check is a single stat again, for every consumer at once. An
        // unpaired or guard-window observation must not become one.
        if(obs->reliable) {
            version.size = obs->size;
            version.mtime_ns = obs->mtime_ns;
            stamp_generation += 1;
        }
        return Verdict::Fresh;
    }
};

}  // namespace clice
