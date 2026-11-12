module clice:unittests.utils.forwarding_database;

import :index.database;
import :support.prelude;

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

    std::expected<void, std::string> write(llvm::ArrayRef<Blob> puts,
                                           llvm::ArrayRef<index::BlobKey> removes) override {
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

    Copy write_compact_copy() override {
        return real->write_compact_copy();
    }

    Swap swap_compact_copy() override {
        return real->swap_compact_copy();
    }

    std::optional<index::PageUsage> usage() override {
        return real->usage();
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

/// A database whose writes fail while `fail` holds: refused, as a full disk
/// or a permission refuses them, or, when the failure `corrupts`, finding
/// the database corrupt, as a damaged page only a write reaches does.
struct FailingDatabase final : ForwardingDatabase {
    FailingDatabase(std::unique_ptr<index::BlobDatabase> real, bool corrupts) :
        ForwardingDatabase(std::move(real)), corrupts(corrupts) {}

    std::expected<void, std::string> write(llvm::ArrayRef<Blob> puts,
                                           llvm::ArrayRef<index::BlobKey> removes) override {
        if(!fail) {
            return real->write(puts, removes);
        }
        found_corrupt = corrupts;
        return std::unexpected(corrupts ? "damaged" : "refused");
    }

    bool corrupted() const override {
        return found_corrupt;
    }

    void condemn() override {
        *condemned = true;
        real->condemn();
    }

    bool fail = true;

    bool corrupts;

    bool found_corrupt = false;

    /// Whether the project condemned the database: shared, since the
    /// project replaces a database it condemns.
    std::shared_ptr<bool> condemned = std::make_shared<bool>(false);
};

}  // namespace clice::testing
