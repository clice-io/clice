module clice:project.graph.precompiled;

import :support.failure_record;
import :support.prelude;
import :vfs.cache_store;
import :vfs.file_table;

namespace clice {

/// A build that failed with what it stands on (see Memo): what a failed
/// build read, or for a crash, which reports nothing it read, the inputs
/// known before it ran. It stands while they hold.
template <typename Result>
struct Failed {
    Result result;

    Inputs inputs;
};

/// What the entries of PCHs and PCMs share: the product in the store, the one
/// it replaced, what it was built from, and the failure of the last build.
template <typename Failure>
struct Precompiled {
    /// The product's name in the store: the entry's key and a nonce of the
    /// build that produced it. A rebuild publishes under a new name, never
    /// over the old product, which a worker or a compile may still have
    /// mapped — Windows refuses to replace a mapped file — and the record
    /// names the very bytes it describes, whatever a crash before the
    /// metadata flush or another writer of the store left behind.
    std::string blob;

    /// The product the latest build replaced, kept for one more build: a
    /// consumer that took its path just before the replacement may not have
    /// opened it yet.
    std::string previous;

    /// The built product; empty when there is none: after a failure, while
    /// a rebuild runs, and once it left the store.
    std::string path;

    /// What the build read and looked for, what it was built over included.
    Inputs inputs;

    /// The last build's failure; none since a success.
    std::optional<Failed<Failure>> failure;

    /// The last build's failure while what it stands on holds; one that no
    /// longer does is forgotten.
    const Failure* standing_failure(FileTable& files) {
        if(failure && !inputs_hold(files, failure->inputs)) {
            failure.reset();
        }
        return failure ? &failure->result : nullptr;
    }

    /// The product no longer describes the entry: it moves to `previous`,
    /// the one before leaving the store's namespace `ns`, and the entry names
    /// no product.
    void retire(CacheStore* store, llvm::StringRef ns) {
        if(!blob.empty()) {
            if(!previous.empty() && store) {
                store->invalidate(ns, previous);
            }
            previous = std::exchange(blob, {});
        }
        path.clear();
        inputs.clear();
    }

    /// A new build committed at `path` under `blob`, built from `inputs`.
    void publish(CacheStore* store,
                 llvm::StringRef ns,
                 std::string blob,
                 std::string path,
                 Inputs inputs) {
        retire(store, ns);
        this->blob = std::move(blob);
        this->path = std::move(path);
        this->inputs = std::move(inputs);
        failure.reset();
    }

    /// Both products leave the store's namespace `ns`; the failure stays.
    void discard(CacheStore* store, llvm::StringRef ns) {
        for(auto* name: {&blob, &previous}) {
            if(!name->empty() && store) {
                store->invalidate(ns, *name);
            }
            name->clear();
        }
        path.clear();
        inputs.clear();
    }
};

/// Length-prefix each part (so embedded NULs cannot create colliding
/// splits) and return the 32-hex xxh3_128bits cache key.
std::string cache_key(llvm::ArrayRef<llvm::StringRef> parts);

/// The name in the store of a new build under `key`: the key and a nonce,
/// so a rebuild publishes beside the product it replaces, never over it.
std::string new_blob(llvm::StringRef key);

/// The record of a build that failed with `cause`: `death` is the identity
/// of the worker death it crashed, empty for errors in the user's code.
FailureRecord failed_build(llvm::StringRef death, std::string cause);

}  // namespace clice
