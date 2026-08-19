#include "index/storage.h"

#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

namespace {

constexpr llvm::StringLiteral index_lock_name = "index.lock";

llvm::StringRef namespace_of(IndexBlobKind kind) {
    switch(kind) {
        case IndexBlobKind::Shard: return "index";
        case IndexBlobKind::Manifest: return "index-manifest";
        case IndexBlobKind::Global: return "index-global";
        case IndexBlobKind::Cdb: return "index-cdb";
    }
    std::unreachable();
}

class FsIndexStorage final : public IndexStorage {
public:
    FsIndexStorage(CacheStore& store, int lock_fd) : store(store), lock_fd(lock_fd) {
        for(auto kind: {IndexBlobKind::Shard,
                        IndexBlobKind::Manifest,
                        IndexBlobKind::Global,
                        IndexBlobKind::Cdb}) {
            store.register_namespace({
                .name = std::string(namespace_of(kind)),
                .extension = ".idx",
                .policy = CachePolicy::Persistent,
            });
        }
    }

    ~FsIndexStorage() override {
        if(lock_fd != -1) {
            llvm::sys::fs::unlockFile(lock_fd);
            llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
        }
    }

    std::unique_ptr<llvm::MemoryBuffer> read(IndexBlobKind kind, llvm::StringRef key) override {
        auto path = store.lookup(namespace_of(kind), key);
        if(!path) {
            return nullptr;
        }
        auto buffer = llvm::MemoryBuffer::getFile(*path);
        if(!buffer) {
            return nullptr;
        }
        return std::move(*buffer);
    }

    bool contains(IndexBlobKind kind, llvm::StringRef key) override {
        return store.lookup(namespace_of(kind), key).has_value();
    }

    llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> batch) override {
        // Batch order encodes dependency (shards → manifests → global →
        // CDB snapshot), so the first failure fails the rest of the batch:
        // continuing would publish an entry whose prerequisites never
        // landed — e.g. a CDB snapshot vouching for a global that failed —
        // and load paths only tolerate a committed prefix, the crash shape.
        auto fail_from = [&](std::size_t i) {
            llvm::SmallVector<std::size_t> failed;
            for(; i < batch.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        };
        for(std::size_t i = 0; i < batch.size(); i += 1) {
            auto& blob = batch[i];
            auto ns = namespace_of(blob.kind);
            auto pending = store.begin_store(ns, blob.key);
            std::error_code ec;
            llvm::raw_fd_ostream os(pending.tmp_path, ec);
            if(ec) {
                LOG_WARN("Failed to write index blob {}/{}: {}", ns, blob.key, ec.message());
                return fail_from(i);
            }
            os.write(blob.bytes.data(), blob.bytes.size());
            os.close();
            // A truncated blob (disk full) must never be committed: the
            // namespaces are Persistent, so it would be served forever.
            if(os.has_error()) {
                LOG_WARN("Failed to write index blob {}/{}: {}",
                         ns,
                         blob.key,
                         os.error().message());
                os.clear_error();
                return fail_from(i);
            }
            if(auto committed = store.commit(std::move(pending)); !committed) {
                LOG_WARN("Failed to commit index blob {}/{}: {}",
                         ns,
                         blob.key,
                         committed.error().message());
                return fail_from(i);
            }
        }
        return {};
    }

    void remove(IndexBlobKind kind, llvm::StringRef key) override {
        store.invalidate(namespace_of(kind), key);
    }

    void for_each_key(IndexBlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        store.for_each_key(namespace_of(kind), fn);
    }

private:
    CacheStore& store;
    int lock_fd;
};

}  // namespace

std::unique_ptr<IndexStorage> make_fs_index_storage(CacheStore& store) {
    int lock_fd = -1;
    if(!store.read_only()) {
        // Atomic per-blob replacement cannot serialize the mutable
        // global/manifest lineage: two writers (an LSP server plus a batch
        // `clice index`) derive the same next generation from the same
        // loaded state, so a manifest written by one passes the other's
        // generation pin with FileVersion ids allocated against a different
        // table, loading rows under the wrong files. An OS advisory lock
        // dies with its process, so a crash leaves nothing stale behind.
        auto lock_path = path::join(store.base_dir(), index_lock_name);
        if(auto ec = llvm::sys::fs::openFileForReadWrite(lock_path,
                                                         lock_fd,
                                                         llvm::sys::fs::CD_OpenAlways,
                                                         llvm::sys::fs::OF_None)) {
            LOG_WARN("Failed to open the index writer lock {}: {}", lock_path, ec.message());
            return nullptr;
        }
        if(llvm::sys::fs::tryLockFile(lock_fd)) {
            LOG_WARN(
                "Another clice process is writing the index cache at {}; "
                "index persistence is disabled for this process",
                store.base_dir());
            llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
            return nullptr;
        }
    }
    return std::make_unique<FsIndexStorage>(store, lock_fd);
}

}  // namespace clice::index
