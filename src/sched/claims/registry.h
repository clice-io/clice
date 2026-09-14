#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "semantic/content.h"
#include "vfs/file_table.h"
#include "worker/protocol.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// The master's memory of which keys some run has checked, is checking, or
/// still owes: the cross-TU deduplication state. Keys are opaque bytes a
/// consumer chose (tidy: a unit's content and its instantiation elements;
/// a later index consumer: a file's shard digest), namespaced by purpose
/// and by the consumer's configuration fingerprint.
///
/// A claim is a grant tied to a run attempt: `land` turns everything the
/// attempt was granted into done, `release` gives it back — a run that
/// dies, is preempted or fails must not leave its keys claimed forever.
/// Units are granted once; an element (an instantiation of a granted
/// unit) is granted to the first attempt that brings it, later attempts
/// re-check only their new elements. Every key remembers which files
/// asked for it, so a key whose grants all failed can be re-run from one
/// of them at the end of a sweep.
class ClaimRegistry {
public:
    struct Key {
        worker::ClaimPurpose purpose;
        std::uint64_t fingerprint;
        Fid file;
        ContentHash key;
        /// Which of the file's units with this hash: two declarations
        /// written identically in one file are two keys.
        std::uint32_t ordinal = 0;

        bool operator==(const Key&) const = default;
    };

    /// Answer a run's claim: grant what nobody covers yet, record the rest.
    /// `requester` is the TU the attempt runs, for re-runs. The result's
    /// `checked` bits are the caller's business and come back untouched.
    worker::ClaimResult claim(std::uint64_t attempt,
                              Fid requester,
                              const worker::ClaimParams& params,
                              llvm::ArrayRef<Fid> files);

    /// The attempt produced its findings: its grants are done.
    void land(std::uint64_t attempt);

    /// The attempt produced nothing: its grants are available again.
    void release(std::uint64_t attempt);

    /// Keys granted to no successful attempt although some run offered
    /// them, with the TUs that offered what is owed — what a sweep re-runs
    /// before it ends. An owed element names only the TUs that
    /// materialize it: the others cannot check it.
    struct Unfinished {
        Key key;
        llvm::SmallVector<Fid, 2> requesters;
    };

    std::vector<Unfinished> unfinished() const;

    /// The (purpose, fingerprint) namespace of a consumer configuration.
    static std::uint64_t fingerprint_of(llvm::StringRef text);

private:
    enum class State : std::uint8_t {
        Unclaimed,
        Claimed,
        Done,
    };

    struct Entry {
        State state = State::Unclaimed;
        std::uint64_t attempt = 0;
        llvm::DenseSet<ContentHash> elements_done;
        llvm::DenseSet<ContentHash> elements_pending;
        llvm::DenseSet<ContentHash> elements_seen;
        llvm::SmallVector<Fid, 2> requesters;
        llvm::DenseMap<ContentHash, llvm::SmallVector<Fid, 2>> element_requesters;
    };

    struct Grant {
        Key key;
        bool unit;
        llvm::SmallVector<ContentHash, 2> elements;
    };

    llvm::DenseMap<Key, Entry> entries;
    llvm::DenseMap<std::uint64_t, std::vector<Grant>> grants;
};

}  // namespace clice

template <>
struct llvm::DenseMapInfo<clice::ContentHash> {
    static clice::ContentHash getEmptyKey() {
        return {.low = ~0ull, .high = ~0ull};
    }

    static clice::ContentHash getTombstoneKey() {
        return {.low = ~0ull - 1, .high = ~0ull};
    }

    static unsigned getHashValue(clice::ContentHash hash) {
        return static_cast<unsigned>(hash.low ^ (hash.high >> 32));
    }

    static bool isEqual(clice::ContentHash a, clice::ContentHash b) {
        return a == b;
    }
};

template <>
struct llvm::DenseMapInfo<clice::ClaimRegistry::Key> {
    using Key = clice::ClaimRegistry::Key;

    static Key getEmptyKey() {
        return {.purpose = clice::worker::ClaimPurpose::Tidy,
                .fingerprint = ~0ull,
                .file = {},
                .key = DenseMapInfo<clice::ContentHash>::getEmptyKey()};
    }

    static Key getTombstoneKey() {
        return {.purpose = clice::worker::ClaimPurpose::Tidy,
                .fingerprint = ~0ull,
                .file = {},
                .key = DenseMapInfo<clice::ContentHash>::getTombstoneKey()};
    }

    static unsigned getHashValue(const Key& key) {
        return static_cast<unsigned>(key.key.low ^ key.key.high ^ key.fingerprint ^
                                     (static_cast<std::uint64_t>(key.file.raw) << 17) ^
                                     (static_cast<std::uint64_t>(key.ordinal) << 40));
    }

    static bool isEqual(const Key& a, const Key& b) {
        return a == b;
    }
};
