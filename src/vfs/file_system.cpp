#include "vfs/file_system.h"

#include <mutex>
#include <string>
#include <tuple>

#include "support/filesystem.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/xxhash.h"

namespace clice::vfs {

namespace {

/// Read an open file whole, served as `as` asks.
llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> load(llvm::sys::fs::file_t handle,
                                                        const llvm::Twine& name,
                                                        Read as) {
    auto buffer = llvm::MemoryBuffer::getOpenFile(handle,
                                                  name,
                                                  /*FileSize=*/-1,
                                                  /*RequiresNullTerminator=*/true,
                                                  /*IsVolatile=*/as != Read::Mapped);
    if(!buffer || as != Read::Text) {
        return buffer;
    }
    auto text = without_bom((*buffer)->getBuffer());
    if(text.size() == (*buffer)->getBufferSize()) {
        return buffer;
    }
    return llvm::MemoryBuffer::getMemBufferCopy(text, (*buffer)->getBufferIdentifier());
}

bool read_starts_with_bom(llvm::StringRef path) {
    auto handle = llvm::sys::fs::openNativeFileForRead(path);
    if(!handle) {
        llvm::consumeError(handle.takeError());
        return false;
    }
    char head[3];
    auto read = llvm::sys::fs::readNativeFile(*handle, head);
    llvm::sys::fs::closeFile(*handle);
    if(!read) {
        llvm::consumeError(read.takeError());
        return false;
    }
    return *read == 3 && without_bom(llvm::StringRef(head, 3)).empty();
}

/// Whether the file a status describes starts with the mark, peeked once
/// per (file, size, mtime): adding or removing the mark changes the size,
/// so a verdict holds as long as clang's own size-and-mtime check of what
/// it read.
bool starts_with_bom(const llvm::vfs::Status& status, llvm::StringRef path) {
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::int64_t>;
    static std::mutex mutex;
    static llvm::DenseMap<Key, bool> verdicts;
    Key key{status.getUniqueID().getDevice(),
            status.getUniqueID().getFile(),
            status.getSize(),
            status.getLastModificationTime().time_since_epoch().count()};
    {
        std::lock_guard lock(mutex);
        if(auto it = verdicts.find(key); it != verdicts.end()) {
            return it->second;
        }
    }
    auto verdict = read_starts_with_bom(path);
    std::lock_guard lock(mutex);
    verdicts.try_emplace(key, verdict);
    return verdict;
}

/// A source file a compile opened: its text, read once through the handle
/// it was opened by, the size its status reports agreeing with the text.
class TextFile : public llvm::vfs::File {
public:
    TextFile(llvm::sys::fs::file_t handle, std::string name, std::string real_name) :
        handle(handle), name(std::move(name)), real_name(std::move(real_name)) {}

    ~TextFile() override {
        close();
    }

    llvm::ErrorOr<llvm::vfs::Status> status() override {
        llvm::sys::fs::file_status status;
        if(auto error = llvm::sys::fs::status(handle, status)) {
            return error;
        }
        auto result = llvm::vfs::Status::copyWithNewName(status, name);
        if(result.getType() != llvm::sys::fs::file_type::regular_file) {
            return result;
        }
        if(auto error = load_text()) {
            return error;
        }
        return llvm::vfs::Status::copyWithNewSize(result, text->getBufferSize());
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
        getBuffer(const llvm::Twine&, int64_t, bool, bool) override {
        if(auto error = load_text()) {
            return error;
        }
        return std::move(text);
    }

    llvm::ErrorOr<std::string> getName() override {
        return real_name.empty() ? name : real_name;
    }

    std::error_code close() override {
        if(handle == llvm::sys::fs::kInvalidFile) {
            return {};
        }
        auto error = llvm::sys::fs::closeFile(handle);
        handle = llvm::sys::fs::kInvalidFile;
        return error;
    }

private:
    std::error_code load_text() {
        if(text) {
            return {};
        }
        auto loaded = load(handle, name, Read::Text);
        if(!loaded) {
            return loaded.getError();
        }
        text = std::move(*loaded);
        return {};
    }

    llvm::sys::fs::file_t handle;
    std::string name;
    std::string real_name;
    std::unique_ptr<llvm::MemoryBuffer> text;
};

}  // namespace

std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::error_code> read(llvm::StringRef path,
                                                                         Read as) {
    auto handle = llvm::sys::fs::openNativeFileForRead(path);
    if(!handle) {
        return std::unexpected(llvm::errorToErrorCode(handle.takeError()));
    }
    auto close = llvm::make_scope_exit([&] { llvm::sys::fs::closeFile(*handle); });
    auto buffer = load(*handle, path, as);
    if(!buffer) {
        return std::unexpected(buffer.getError());
    }
    return std::move(*buffer);
}

std::expected<ObservedFile, std::error_code> read_observed(llvm::StringRef path) {
    auto handle = llvm::sys::fs::openNativeFileForRead(path);
    if(!handle) {
        return std::unexpected(llvm::errorToErrorCode(handle.takeError()));
    }
    auto close = llvm::make_scope_exit([&] { llvm::sys::fs::closeFile(*handle); });

    llvm::sys::fs::file_status before;
    bool have_before = !llvm::sys::fs::status(*handle, before);

    // The bytes must be a snapshot taken between the two fstats — a mapped
    // buffer would keep tracking the file after the post-fstat, unpairing
    // hash and stat; Text never maps.
    auto buffer = load(*handle, path, Read::Text);
    if(!buffer) {
        return std::unexpected(buffer.getError());
    }

    ObservedFile result;
    result.content = std::move(*buffer);
    result.obs.hash = llvm::xxh3_64bits(result.content->getBuffer());

    llvm::sys::fs::file_status after;
    if(llvm::sys::fs::status(*handle, after)) {
        return result;
    }
    result.obs.size = after.getSize();
    result.obs.mtime_ns = fs::mtime_ns(after);
    result.obs.uid_device = after.getUniqueID().getDevice();
    result.obs.uid_file = after.getUniqueID().getFile();
    result.obs.paired = have_before && before.getSize() == after.getSize() &&
                        fs::mtime_ns(before) == result.obs.mtime_ns;
    result.obs.reliable = result.obs.paired && fs::settled(result.obs.mtime_ns);
    return result;
}

llvm::ErrorOr<llvm::vfs::Status> View::status(const llvm::Twine& path) {
    auto status = getUnderlyingFS().status(path);
    if(!status || status->getType() != llvm::sys::fs::file_type::regular_file ||
       status->getSize() < 3) {
        return status;
    }
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(getUnderlyingFS().makeAbsolute(absolute) || !starts_with_bom(*status, absolute)) {
        return status;
    }
    return llvm::vfs::Status::copyWithNewSize(*status, status->getSize() - 3);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> View::openFileForRead(const llvm::Twine& path) {
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(auto error = getUnderlyingFS().makeAbsolute(absolute)) {
        return error;
    }
    llvm::SmallString<256> real_name;
    auto handle =
        llvm::sys::fs::openNativeFileForRead(absolute, llvm::sys::fs::OF_None, &real_name);
    if(!handle) {
        return llvm::errorToErrorCode(handle.takeError());
    }
    return std::make_unique<TextFile>(*handle, path.str(), real_name.str().str());
}

}  // namespace clice::vfs
