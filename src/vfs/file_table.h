#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>

#include "support/filesystem.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
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
    bool paired = false;
    bool reliable = false;
};

/// A completed observed read: the observation plus the bytes it hashed.
struct ObservedFile {
    DiskObservation obs;
    std::unique_ptr<llvm::MemoryBuffer> content;
};

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
struct FileTable {
    llvm::BumpPtrAllocator allocator;
    llvm::SmallVector<llvm::StringRef> spellings;
    llvm::StringMap<std::uint32_t> ids;

    std::uint32_t intern(llvm::StringRef path) {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);

        auto [it, inserted] = ids.try_emplace(path, spellings.size());
        if(inserted) {
            // Allocate with null terminator so that resolve().data() is safe
            // to use as const char* (e.g. in MemoryBuffer::getFile which calls strlen).
            const std::size_t n = path.size();
            char* buf = allocator.Allocate<char>(n + 1);
            std::copy(path.begin(), path.end(), buf);
            buf[n] = '\0';
            spellings.push_back(llvm::StringRef(buf, n));
        }
        return it->second;
    }

    llvm::StringRef resolve(std::uint32_t fid) const {
        assert(fid < spellings.size());
        return spellings[fid];
    }

    /// Look up a path without interning it, applying the same
    /// normalization as intern().
    std::optional<std::uint32_t> find(llvm::StringRef path) const {
        llvm::SmallString<256> storage;
        path = path::canonical(path, storage);
        auto it = ids.find(path);
        if(it == ids.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Last reliable same-source {stat, hash} pair per fid: the shared
    /// baseline every consumer's staleness check draws from and repairs.
    /// Consumer-specific observation state (what a consumer has *seen*,
    /// e.g. the tracker's last-reported baseline) stays with the
    /// consumer — sharing it would swallow events.
    llvm::DenseMap<std::uint32_t, DiskObservation> disk_states;

    /// The cached hash of exactly this (size, mtime), or nullopt when
    /// someone must read. Equality against the shared pair, never a
    /// watermark: the hash is "the hash of the bytes that had this
    /// stat", nothing else.
    std::optional<std::uint64_t> cached_hash(std::uint32_t fid,
                                             std::uint64_t size,
                                             std::int64_t mtime_ns) const {
        auto it = disk_states.find(fid);
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
    /// read()) as the fid's shared pair. Unpaired reads carry a true
    /// hash but no stat proof, so they never enter.
    void observe(std::uint32_t fid, const DiskObservation& obs) {
        if(obs.reliable) {
            disk_states[fid] = obs;
        }
    }

    /// Read the file under the pairing discipline and refresh the shared
    /// pair. nullopt = unreadable right now (the pair is left untouched;
    /// what a failed read means is the caller's policy).
    std::optional<DiskObservation> read(std::uint32_t fid) {
        auto observed = read_file_observed(resolve(fid).data());
        if(!observed) {
            return std::nullopt;
        }
        observe(fid, observed->obs);
        return observed->obs;
    }

    /// Stat the file and produce a same-source observation of its
    /// current content. nullopt = missing or unreadable.
    std::optional<DiskObservation> current(std::uint32_t fid) {
        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(resolve(fid), status)) {
            return std::nullopt;
        }
        return observe_for(fid, status.getSize(), fs::mtime_ns(status));
    }

    /// The two-layer primitive: a same-source observation for a live
    /// stat the caller just took — the shared pair when it matches by
    /// equality, else a real read (which repairs the pair for every
    /// later consumer; its observation may describe a newer stat than
    /// the caller's, which is then simply newer truth). nullopt =
    /// unreadable right now.
    std::optional<DiskObservation> observe_for(std::uint32_t fid,
                                               std::uint64_t size,
                                               std::int64_t mtime_ns) {
        if(auto hash = cached_hash(fid, size, mtime_ns)) {
            return DiskObservation{.size = size,
                                   .mtime_ns = mtime_ns,
                                   .hash = *hash,
                                   .paired = true,
                                   .reliable = true};
        }
        return read(fid);
    }
};

}  // namespace clice
