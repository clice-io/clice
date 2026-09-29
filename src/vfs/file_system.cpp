#include "vfs/file_system.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

#include "support/filesystem.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/xxhash.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "llvm/Support/WindowsError.h"

namespace llvm::sys::windows {

// Declared by llvm/Support/Windows/WindowsSupport.h, which pins
// _WIN32_WINNT below the version that declares GetFileInformationByName.
std::error_code widenPath(const Twine& path8,
                          SmallVectorImpl<wchar_t>& path16,
                          size_t max_path_len = MAX_PATH);

}  // namespace llvm::sys::windows
#endif

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

using StatusResult = std::expected<llvm::sys::fs::file_status, std::error_code>;

#ifdef _WIN32

llvm::sys::fs::file_status make_status(DWORD attributes,
                                       std::uint64_t last_access,
                                       std::uint64_t last_write,
                                       std::uint64_t size,
                                       std::uint64_t volume,
                                       const FILE_ID_128& id,
                                       DWORD links) {
    auto type = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? llvm::sys::fs::file_type::directory_file
                                                        : llvm::sys::fs::file_type::regular_file;
    auto perms = (attributes & FILE_ATTRIBUTE_READONLY)
                     ? llvm::sys::fs::all_read | llvm::sys::fs::all_exe
                     : llvm::sys::fs::all_all;
    // All 128 bits: ReFS file IDs do not fit in 64.
    auto hash = static_cast<std::uint64_t>(
        llvm::hash_combine_range(std::begin(id.Identifier), std::end(id.Identifier)));
    return llvm::sys::fs::file_status(type,
                                      perms,
                                      links,
                                      static_cast<std::uint32_t>(last_access >> 32),
                                      static_cast<std::uint32_t>(last_access),
                                      static_cast<std::uint32_t>(last_write >> 32),
                                      static_cast<std::uint32_t>(last_write),
                                      static_cast<std::uint32_t>(volume),
                                      static_cast<std::uint32_t>(size >> 32),
                                      static_cast<std::uint32_t>(size),
                                      hash);
}

std::uint64_t filetime(const FILETIME& time) {
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

StatusResult handle_status(HANDLE handle) {
    switch(::GetFileType(handle)) {
        case FILE_TYPE_DISK: break;
        case FILE_TYPE_CHAR:
            return llvm::sys::fs::file_status(llvm::sys::fs::file_type::character_file);
        case FILE_TYPE_PIPE: return llvm::sys::fs::file_status(llvm::sys::fs::file_type::fifo_file);
        default: return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    FILE_REMOTE_PROTOCOL_INFO remote;
    if(::GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote))) {
        llvm::sys::fs::file_status status;
        if(auto error = llvm::sys::fs::status(handle, status)) {
            return std::unexpected(error);
        }
        return status;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if(!::GetFileInformationByHandle(handle, &info)) {
        return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    FILE_ID_INFO id;
    if(!::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        // A file system with 64-bit IDs only: the by-name query reports
        // the same ID widened.
        id = {.VolumeSerialNumber = info.dwVolumeSerialNumber, .FileId = {}};
        auto index = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        std::memcpy(id.FileId.Identifier, &index, sizeof(index));
    }
    return make_status(info.dwFileAttributes,
                       filetime(info.ftLastAccessTime),
                       filetime(info.ftLastWriteTime),
                       (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
                       id.VolumeSerialNumber,
                       id.FileId,
                       info.nNumberOfLinks);
}

using GetFileInformationByNameFn = BOOL(WINAPI*)(PCWSTR, FILE_INFO_BY_NAME_CLASS, PVOID, ULONG);

/// Null before Windows 11 24H2.
GetFileInformationByNameFn by_name() {
    static auto fn = reinterpret_cast<GetFileInformationByNameFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernelbase.dll"), "GetFileInformationByName"));
    return fn;
}

/// The errors a by-name query reports only for a path that names nothing;
/// anything else it may report for a file an open would reach (the list
/// CPython trusts).
bool names_nothing(DWORD error) {
    switch(error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NOT_READY:
        case ERROR_BAD_NET_NAME:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_NAME: return true;
        default: return false;
    }
}

StatusResult by_path(llvm::StringRef path) {
    llvm::SmallVector<wchar_t, 256> wide;
    if(auto error = llvm::sys::windows::widenPath(path, wide)) {
        return std::unexpected(error);
    }
    wide.push_back(L'\0');
    if(auto query = by_name()) {
        // Not in the SDK's user-mode headers: the device is on another
        // machine, where file IDs can be reused.
        constexpr DWORD remote_device = 0x10;
        FILE_STAT_BASIC_INFORMATION info;
        if(query(wide.data(), FileStatBasicByNameInfo, &info, sizeof(info))) {
            // A reparse point is not followed, a remote file is asked through
            // a handle as well.
            if(!(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
               !(info.DeviceCharacteristics & remote_device)) {
                return make_status(info.FileAttributes,
                                   info.LastAccessTime.QuadPart,
                                   info.LastWriteTime.QuadPart,
                                   info.EndOfFile.QuadPart,
                                   info.VolumeSerialNumber.QuadPart,
                                   info.FileId128,
                                   info.NumberOfLinks);
            }
        } else if(auto error = ::GetLastError(); names_nothing(error)) {
            return std::unexpected(llvm::mapWindowsError(error));
        }
    }
    HANDLE handle = ::CreateFileW(wide.data(),
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(handle); });
    return handle_status(handle);
}

#else

StatusResult handle_status(llvm::sys::fs::file_t handle) {
    llvm::sys::fs::file_status status;
    if(auto error = llvm::sys::fs::status(handle, status)) {
        return std::unexpected(error);
    }
    return status;
}

#endif

bool same_file_state(const llvm::sys::fs::file_status& a, const llvm::sys::fs::file_status& b) {
    return a.getSize() == b.getSize() &&
           a.getLastModificationTime() == b.getLastModificationTime() &&
           a.getUniqueID() == b.getUniqueID();
}

/// Serves a buffer the process keeps to one compile.
class SharedBuffer : public llvm::MemoryBuffer {
public:
    SharedBuffer(std::shared_ptr<const llvm::MemoryBuffer> owner, bool requires_terminator) :
        owner(std::move(owner)) {
        init(this->owner->getBufferStart(), this->owner->getBufferEnd(), requires_terminator);
    }

    llvm::StringRef getBufferIdentifier() const override {
        return owner->getBufferIdentifier();
    }

    BufferKind getBufferKind() const override {
        return owner->getBufferKind();
    }

private:
    std::shared_ptr<const llvm::MemoryBuffer> owner;
};

/// Source texts kept across the compiles of a process, by the path they
/// were opened under: every compile reads each header it includes, and
/// most are the bytes the compile before read. A text is served again
/// while a status of its file matches the one it was read under — size,
/// mtime and ID, the evidence the master's own change checks rest on — and
/// only a text read in one piece whose mtime lay outside the guard window
/// is kept at all.
class TextCache {
public:
    struct Entry {
        llvm::sys::fs::file_status status;
        std::string real_name;
        std::shared_ptr<const llvm::MemoryBuffer> text;
        std::uint64_t used = 0;
    };

    std::optional<Entry> find(llvm::StringRef path, const llvm::sys::fs::file_status& status) {
        std::lock_guard lock(mutex);
        auto it = entries.find(path);
        if(it == entries.end() || !same_file_state(it->second.status, status)) {
            return std::nullopt;
        }
        clock += 1;
        it->second.used = clock;
        return it->second;
    }

    void insert(llvm::StringRef path, Entry entry) {
        if(entry.text->getBufferSize() > budget) {
            return;
        }
        std::lock_guard lock(mutex);
        clock += 1;
        entry.used = clock;
        auto [it, inserted] = entries.try_emplace(path);
        if(!inserted) {
            bytes -= cost(*it);
        }
        it->second = std::move(entry);
        bytes += cost(*it);
        while(bytes > budget) {
            auto oldest = std::ranges::min_element(entries, {}, [](const auto& candidate) {
                return candidate.second.used;
            });
            bytes -= cost(*oldest);
            entries.erase(oldest);
        }
    }

private:
    constexpr static std::size_t budget = 128 << 20;

    /// An entry's memory, its bookkeeping included: a cache of empty
    /// headers is not free.
    static std::size_t cost(const llvm::StringMapEntry<Entry>& entry) {
        return entry.getKeyLength() + entry.second.real_name.size() +
               entry.second.text->getBufferSize() + 256;
    }

    std::mutex mutex;
    llvm::StringMap<Entry> entries;
    std::size_t bytes = 0;
    std::uint64_t clock = 0;
};

TextCache& texts() {
    static TextCache cache;
    return cache;
}

/// Mappings of the artifacts keep_mapped() named, most recently named
/// first. A mapping is used while the file stats as it did when mapped.
class MappedArtifacts {
public:
    void keep(llvm::StringRef path) {
        std::lock_guard lock(mutex);
        auto it = std::ranges::find(entries, path, &Entry::path);
        if(it == entries.end()) {
            entries.insert(entries.begin(), Entry{.path = path.str()});
            if(entries.size() > capacity) {
                entries.pop_back();
            }
        } else {
            std::rotate(entries.begin(), it, it + 1);
        }
    }

    /// The mapping of the file `handle` opens at `path`, kept when `path`
    /// was named; nullptr when it was not.
    std::shared_ptr<const llvm::MemoryBuffer> map(llvm::StringRef path,
                                                  llvm::sys::fs::file_t handle,
                                                  const llvm::sys::fs::file_status& status) {
        std::lock_guard lock(mutex);
        auto it = std::ranges::find(entries, path, &Entry::path);
        if(it == entries.end()) {
            return nullptr;
        }
        if(!it->buffer || !same_file_state(it->status, status)) {
            auto mapped = llvm::MemoryBuffer::getOpenFile(handle,
                                                          path,
                                                          status.getSize(),
                                                          /*RequiresNullTerminator=*/false,
                                                          /*IsVolatile=*/false);
            if(!mapped) {
                return nullptr;
            }
            it->status = status;
            it->buffer = std::move(*mapped);
        }
        return it->buffer;
    }

private:
    struct Entry {
        std::string path;
        llvm::sys::fs::file_status status;
        std::shared_ptr<const llvm::MemoryBuffer> buffer;
    };

    /// PCHs of the documents a worker serves in turn. On Windows a kept
    /// mapping also keeps a retracted PCH on disk until the next start.
    constexpr static std::size_t capacity = 8;

    std::mutex mutex;
    std::vector<Entry> entries;
};

MappedArtifacts& mapped() {
    static MappedArtifacts artifacts;
    return artifacts;
}

/// A source file served from the process's texts.
class CachedText : public llvm::vfs::File {
public:
    CachedText(std::string name, TextCache::Entry entry) :
        name(std::move(name)), entry(std::move(entry)) {}

    llvm::ErrorOr<llvm::vfs::Status> status() override {
        auto result = llvm::vfs::Status::copyWithNewName(entry.status, name);
        return llvm::vfs::Status::copyWithNewSize(result, entry.text->getBufferSize());
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
        getBuffer(const llvm::Twine&, int64_t, bool, bool) override {
        return std::make_unique<SharedBuffer>(entry.text, true);
    }

    llvm::ErrorOr<std::string> getName() override {
        return entry.real_name.empty() ? name : entry.real_name;
    }

    std::error_code close() override {
        return {};
    }

private:
    std::string name;
    TextCache::Entry entry;
};

/// A file a compile opened, answering its status through the handle it
/// was opened by. A source file is served as its text, read once, the size
/// its status reports agreeing with the text; a binary file as its bytes,
/// read the way clang asks.
class DiskFile : public llvm::vfs::File {
public:
    /// `key` is the absolute path the process keeps the file's text or
    /// mapping under.
    DiskFile(llvm::sys::fs::file_t handle,
             std::string name,
             std::string real_name,
             std::string key,
             bool text) :
        handle(handle), name(std::move(name)), real_name(std::move(real_name)), key(std::move(key)),
        is_text(text) {}

    ~DiskFile() override {
        close();
    }

    llvm::ErrorOr<llvm::vfs::Status> status() override {
        auto status = handle_status(handle);
        if(!status) {
            return status.error();
        }
        auto result = llvm::vfs::Status::copyWithNewName(*status, name);
        if(!is_text || result.getType() != llvm::sys::fs::file_type::regular_file) {
            return result;
        }
        if(auto error = load_text()) {
            return error;
        }
        return llvm::vfs::Status::copyWithNewSize(result, text->getBufferSize());
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(const llvm::Twine& buffer_name,
                                                                 int64_t size,
                                                                 bool requires_terminator,
                                                                 bool is_volatile) override {
        if(!is_text) {
            if(!requires_terminator && !is_volatile) {
                if(auto status = handle_status(handle)) {
                    if(auto kept = mapped().map(key, handle, *status)) {
                        return std::make_unique<SharedBuffer>(std::move(kept), false);
                    }
                }
            }
            return llvm::MemoryBuffer::getOpenFile(handle,
                                                   buffer_name,
                                                   size,
                                                   requires_terminator,
                                                   is_volatile);
        }
        if(auto error = load_text()) {
            return error;
        }
        return std::make_unique<SharedBuffer>(text, true);
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
        auto before = handle_status(handle);
        auto loaded = load(handle, name, Read::Text);
        if(!loaded) {
            return loaded.getError();
        }
        text = std::move(*loaded);
        auto after = handle_status(handle);
        if(before && after && after->type() == llvm::sys::fs::file_type::regular_file &&
           same_file_state(*before, *after) && fs::settled(fs::mtime_ns(*after))) {
            texts().insert(key, {.status = *after, .real_name = real_name, .text = text});
        }
        return {};
    }

    llvm::sys::fs::file_t handle;
    std::string name;
    std::string real_name;
    std::string key;
    bool is_text;
    std::shared_ptr<const llvm::MemoryBuffer> text;
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

std::expected<llvm::sys::fs::file_status, std::error_code> status(llvm::StringRef path) {
#ifdef _WIN32
    return by_path(path);
#else
    llvm::sys::fs::file_status status;
    if(auto error = llvm::sys::fs::status(path, status)) {
        return std::unexpected(error);
    }
    return status;
#endif
}

llvm::ErrorOr<llvm::vfs::Status> View::status(const llvm::Twine& path) {
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(auto error = getUnderlyingFS().makeAbsolute(absolute)) {
        return error;
    }
    auto status = vfs::status(absolute);
    if(!status) {
        return status.error();
    }
    auto result = llvm::vfs::Status::copyWithNewName(*status, path);
    if(result.getType() != llvm::sys::fs::file_type::regular_file || result.getSize() < 3 ||
       !starts_with_bom(result, absolute)) {
        return result;
    }
    return llvm::vfs::Status::copyWithNewSize(result, result.getSize() - 3);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> View::openFileForRead(const llvm::Twine& path) {
    return open(path, true);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
    View::openFileForReadBinary(const llvm::Twine& path) {
    return open(path, false);
}

llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> View::open(const llvm::Twine& path, bool text) {
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(auto error = getUnderlyingFS().makeAbsolute(absolute)) {
        return error;
    }
    if(text) {
        auto status = vfs::status(absolute);
        if(!status) {
            return status.error();
        }
        if(auto kept = texts().find(absolute, *status)) {
            return std::make_unique<CachedText>(path.str(), std::move(*kept));
        }
    }
    llvm::SmallString<256> real_name;
    auto handle =
        llvm::sys::fs::openNativeFileForRead(absolute, llvm::sys::fs::OF_None, &real_name);
    if(!handle) {
        return llvm::errorToErrorCode(handle.takeError());
    }
    return std::make_unique<DiskFile>(*handle,
                                      path.str(),
                                      real_name.str().str(),
                                      absolute.str().str(),
                                      text);
}

void keep_mapped(llvm::StringRef path) {
    mapped().keep(path);
}

}  // namespace clice::vfs
