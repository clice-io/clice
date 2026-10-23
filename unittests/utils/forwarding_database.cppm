module;

#include "support/prelude.h"

module clice:unittests.utils.forwarding_database;

import :index.database;

namespace clice::testing {

/// A database passing every call to `real`: a test double overrides the
/// calls it changes.
struct ForwardingDatabase : index::BlobDatabase {
    explicit ForwardingDatabase(std::unique_ptr<index::BlobDatabase> real) :
        real(std::move(real)) {}

    index::ReadBlob read(index::BlobKind kind, llvm::StringRef key) override {
        return real->read(kind, key);
    }

    bool contains(index::BlobKind kind, llvm::StringRef key) override {
        return real->contains(kind, key);
    }

    bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<index::BlobKey> removes) override {
        return real->write(puts, removes);
    }

    void for_each_key(index::BlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        real->for_each_key(kind, fn);
    }

    void for_each(index::BlobKind kind,
                  llvm::function_ref<bool(llvm::StringRef, index::ReadBlob)> fn) override {
        real->for_each(kind, fn);
    }

    std::uint32_t count(index::BlobKind kind) override {
        return real->count(kind);
    }

    std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
        return real->advance_read_snapshot();
    }

    void retire_old_snapshot() override {
        real->retire_old_snapshot();
    }

    Growth grow() override {
        return real->grow();
    }

    bool corrupted() const override {
        return real->corrupted();
    }

    void condemn() override {
        real->condemn();
    }

    bool read_only() const override {
        return real->read_only();
    }

    std::unique_ptr<index::BlobDatabase> real;
};

}  // namespace clice::testing
