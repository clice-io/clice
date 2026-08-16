#include "index/storage.h"

#include "support/cache_store.h"
#include "support/logging.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::index {

namespace {

llvm::StringRef namespace_of(IndexBlobKind kind) {
    switch(kind) {
        case IndexBlobKind::Shard: return "index";
        case IndexBlobKind::Manifest: return "index-manifest";
        case IndexBlobKind::Global: return "index-global";
    }
    std::unreachable();
}

class FsIndexStorage final : public IndexStorage {
public:
    explicit FsIndexStorage(CacheStore& store) : store(store) {
        for(auto kind: {IndexBlobKind::Shard, IndexBlobKind::Manifest, IndexBlobKind::Global}) {
            store.register_namespace({
                .name = std::string(namespace_of(kind)),
                .extension = ".idx",
                .policy = CachePolicy::Persistent,
            });
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
        llvm::SmallVector<std::size_t> failed;
        for(std::size_t i = 0; i < batch.size(); i += 1) {
            auto& blob = batch[i];
            auto ns = namespace_of(blob.kind);
            auto pending = store.begin_store(ns, blob.key);
            std::error_code ec;
            llvm::raw_fd_ostream os(pending.tmp_path, ec);
            if(ec) {
                LOG_WARN("Failed to write index blob {}/{}: {}", ns, blob.key, ec.message());
                failed.push_back(i);
                continue;
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
                failed.push_back(i);
                continue;
            }
            if(auto committed = store.commit(std::move(pending)); !committed) {
                LOG_WARN("Failed to commit index blob {}/{}: {}",
                         ns,
                         blob.key,
                         committed.error().message());
                failed.push_back(i);
                continue;
            }
        }
        return failed;
    }

    void remove(IndexBlobKind kind, llvm::StringRef key) override {
        store.invalidate(namespace_of(kind), key);
    }

    void for_each_key(IndexBlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        store.for_each_key(namespace_of(kind), fn);
    }

private:
    CacheStore& store;
};

}  // namespace

std::unique_ptr<IndexStorage> make_fs_index_storage(CacheStore& store) {
    return std::make_unique<FsIndexStorage>(store);
}

}  // namespace clice::index
