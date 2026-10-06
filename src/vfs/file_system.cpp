module;

#include "modules/prelude.h"

#ifdef _WIN32
#include "vfs/win32.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/WindowsError.h"
#else
#include <sys/stat.h>
#endif
#include "support/logging.macros.h"

module clice;

import :support.logging;
import :vfs.file_system;
import :vfs.path;

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

/// Whether the file a stamp describes starts with the mark, peeked once
/// per stamp: adding or removing the mark changes the size, so a verdict
/// holds as long as clang's own size-and-mtime check of what it read.
bool starts_with_bom(const Stamp& stamp, llvm::StringRef path) {
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::int64_t, std::int64_t>;
    static std::mutex mutex;
    static llvm::DenseMap<Key, bool> verdicts;
    Key key{stamp.device, stamp.file, stamp.size, stamp.mtime_ns, stamp.ctime_ns};
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

using StatusResult = std::expected<Status, std::error_code>;

#ifdef _WIN32

/// FILETIME ticks (100 ns since 1601) as nanoseconds since the Unix epoch;
/// a time no clock reaches (FAT's zero ChangeTime) wraps rather than
/// overflows.
std::int64_t unix_ns(std::uint64_t ticks) {
    constexpr std::uint64_t unix_epoch = 116'444'736'000'000'000;
    return static_cast<std::int64_t>((ticks - unix_epoch) * 100);
}

Status make_status(DWORD attributes,
                   std::uint64_t last_write,
                   std::uint64_t change,
                   std::uint64_t size,
                   std::uint64_t volume,
                   const FILE_ID_128& id,
                   DWORD links) {
    // All 128 bits: ReFS file IDs do not fit in 64.
    auto hash = static_cast<std::uint64_t>(
        llvm::hash_combine_range(std::begin(id.Identifier), std::end(id.Identifier)));
    return {
        .type = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? llvm::sys::fs::file_type::directory_file
                                                        : llvm::sys::fs::file_type::regular_file,
        .stamp = {.size = size,
                  .mtime_ns = unix_ns(last_write),
                  .ctime_ns = unix_ns(change),
                  .device = static_cast<std::uint32_t>(volume),
                  .file = hash},
        .links = links,
    };
}

std::uint64_t filetime(const FILETIME& time) {
    return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

StatusResult handle_status(HANDLE handle) {
    switch(::GetFileType(handle)) {
        case FILE_TYPE_DISK: break;
        case FILE_TYPE_CHAR: return Status{.type = llvm::sys::fs::file_type::character_file};
        case FILE_TYPE_PIPE: return Status{.type = llvm::sys::fs::file_type::fifo_file};
        default: return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    // The handle query below has no change time.
    FILE_BASIC_INFO basic;
    if(!::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
        return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    FILE_REMOTE_PROTOCOL_INFO remote;
    if(::GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &remote, sizeof(remote))) {
        llvm::sys::fs::file_status status;
        if(auto error = llvm::sys::fs::status(handle, status)) {
            return std::unexpected(error);
        }
        return Status{
            .type = status.type(),
            .stamp = {.size = status.getSize(),
                      .mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      status.getLastModificationTime().time_since_epoch())
                                      .count(),
                      .ctime_ns = unix_ns(basic.ChangeTime.QuadPart),
                      .device = status.getUniqueID().getDevice(),
                      .file = status.getUniqueID().getFile()},
            .links = status.getLinkCount(),
        };
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
                       filetime(info.ftLastWriteTime),
                       basic.ChangeTime.QuadPart,
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
    if(auto error = widen(path, wide)) {
        return std::unexpected(error);
    }
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
                                   info.LastWriteTime.QuadPart,
                                   info.ChangeTime.QuadPart,
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

/// A directory asked about this often within one operation is listed.
constexpr unsigned list_after = 8;

/// A directory with more entries is not listed: the few names asked
/// about it would pay for all of them.
constexpr std::size_t list_limit = 4096;

/// A handle to list `dir` through.
std::expected<HANDLE, std::error_code> open_directory(llvm::StringRef dir) {
    llvm::SmallVector<wchar_t, 256> wide;
    if(auto error = widen(dir, wide)) {
        return std::unexpected(error);
    }
    HANDLE handle = ::CreateFileW(wide.data(),
                                  FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(llvm::mapWindowsError(::GetLastError()));
    }
    return handle;
}

/// Hand each entry of the directory `handle` lists, `.` and `..` left out,
/// to `visit` as the information class `Info` describes it, until `visit`
/// answers false. A failure other than running out of entries is returned.
template <typename Info>
std::error_code for_each_entry(HANDLE handle,
                               FILE_INFO_BY_HANDLE_CLASS restart,
                               FILE_INFO_BY_HANDLE_CLASS next,
                               llvm::function_ref<bool(const Info&, llvm::StringRef)> visit) {
    alignas(8) static thread_local char buffer[64 * 1024];
    auto kind = restart;
    while(::GetFileInformationByHandleEx(handle, kind, buffer, sizeof(buffer))) {
        kind = next;
        for(auto* entry = reinterpret_cast<const Info*>(buffer);;
            entry = reinterpret_cast<const Info*>(reinterpret_cast<const char*>(entry) +
                                                  entry->NextEntryOffset)) {
            std::wstring_view wide(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
            std::string name;
            if(wide != L"." && wide != L".." && llvm::convertWideToUTF8(wide, name) &&
               !visit(*entry, name)) {
                return {};
            }
            if(entry->NextEntryOffset == 0) {
                break;
            }
        }
    }
    auto error = ::GetLastError();
    return error == ERROR_NO_MORE_FILES ? std::error_code() : llvm::mapWindowsError(error);
}

/// The statuses of a directory's entries in the ID scheme by_path() uses,
/// reparse points left out; empty when it cannot be listed.
llvm::StringMap<Status> list_statuses(llvm::StringRef dir) {
    llvm::StringMap<Status> entries;
    auto handle = open_directory(dir);
    if(!handle) {
        return entries;
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(*handle); });
    FILE_REMOTE_PROTOCOL_INFO remote;
    FILE_ID_INFO id;
    if(::GetFileInformationByHandleEx(*handle, FileRemoteProtocolInfo, &remote, sizeof(remote)) ||
       !::GetFileInformationByHandleEx(*handle, FileIdInfo, &id, sizeof(id))) {
        return entries;
    }
    for_each_entry<FILE_ID_EXTD_DIR_INFO>(
        *handle,
        FileIdExtdDirectoryRestartInfo,
        FileIdExtdDirectoryInfo,
        [&](const FILE_ID_EXTD_DIR_INFO& entry, llvm::StringRef name) {
            if(!(entry.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                entries.try_emplace(name,
                                    make_status(entry.FileAttributes,
                                                entry.LastWriteTime.QuadPart,
                                                entry.ChangeTime.QuadPart,
                                                entry.EndOfFile.QuadPart,
                                                id.VolumeSerialNumber,
                                                entry.FileId,
                                                /*links=*/1));
            }
            if(entries.size() > list_limit) {
                entries.clear();
                return false;
            }
            return true;
        });
    return entries;
}

#else

std::int64_t unix_ns(const struct timespec& time) {
    return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

Status make_status(const struct stat& info) {
    using llvm::sys::fs::file_type;
    auto type = S_ISREG(info.st_mode)    ? file_type::regular_file
                : S_ISDIR(info.st_mode)  ? file_type::directory_file
                : S_ISCHR(info.st_mode)  ? file_type::character_file
                : S_ISBLK(info.st_mode)  ? file_type::block_file
                : S_ISFIFO(info.st_mode) ? file_type::fifo_file
                : S_ISSOCK(info.st_mode) ? file_type::socket_file
                                         : file_type::type_unknown;
#ifdef __APPLE__
    auto& mtime = info.st_mtimespec;
    auto& ctime = info.st_ctimespec;
#else
    auto& mtime = info.st_mtim;
    auto& ctime = info.st_ctim;
#endif
    return {
        .type = type,
        .stamp = {.size = static_cast<std::uint64_t>(info.st_size),
                  .mtime_ns = unix_ns(mtime),
                  .ctime_ns = unix_ns(ctime),
                  .device = static_cast<std::uint64_t>(info.st_dev),
                  .file = static_cast<std::uint64_t>(info.st_ino)},
        .links = static_cast<std::uint32_t>(info.st_nlink),
    };
}

StatusResult handle_status(llvm::sys::fs::file_t handle) {
    struct stat info;
    if(::fstat(handle, &info) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return make_status(info);
}

#endif

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
        Status status;
        std::string real_name;
        std::shared_ptr<const llvm::MemoryBuffer> text;
        std::uint64_t used = 0;
    };

    std::optional<Entry> find(llvm::StringRef path, const Status& status) {
        std::lock_guard lock(mutex);
        auto it = entries.find(path);
        if(it == entries.end() || it->second.status.stamp != status.stamp) {
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
        auto key = spelling(path);
        std::lock_guard lock(mutex);
        auto it = std::ranges::find(entries, key, &Entry::path);
        if(it == entries.end()) {
            entries.insert(entries.begin(), Entry{.path = std::move(key)});
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
                                                  const Status& status) {
        auto key = spelling(path);
        std::lock_guard lock(mutex);
        auto it = std::ranges::find(entries, key, &Entry::path);
        if(it == entries.end()) {
            return nullptr;
        }
        if(!it->buffer || it->status.stamp != status.stamp) {
            auto mapped = llvm::MemoryBuffer::getOpenFile(handle,
                                                          path,
                                                          status.stamp.size,
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
    /// The master names a PCH as the store spells it, clang as the
    /// compile's arguments do (on Windows the two mix separators
    /// differently): compare them in one spelling.
    static std::string spelling(llvm::StringRef path) {
        std::string key = path.str();
        path::canonicalize(key);
        return key;
    }

    struct Entry {
        std::string path;
        Status status;
        std::shared_ptr<const llvm::MemoryBuffer> buffer;
    };

    /// PCHs of the documents a worker serves in turn.
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
        return llvm::vfs::Status::copyWithNewSize(entry.status.to_llvm(name),
                                                  entry.text->getBufferSize());
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
        auto result = status->to_llvm(name);
        if(!is_text || !status->is_file()) {
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
        if(before && after && after->is_file() && before->stamp == after->stamp &&
           settled(after->stamp.mtime_ns)) {
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

    auto before = handle_status(*handle);

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

    auto after = handle_status(*handle);
    if(!after) {
        return result;
    }
    result.obs.stamp = after->stamp;
    result.obs.paired = before && before->stamp == after->stamp;
    result.obs.reliable = result.obs.paired && settled(after->stamp.mtime_ns);
    return result;
}

llvm::vfs::Status Status::to_llvm(llvm::StringRef name) const {
    return llvm::vfs::Status(name,
                             llvm::sys::fs::UniqueID(stamp.device, stamp.file),
                             llvm::sys::TimePoint<>(std::chrono::nanoseconds(stamp.mtime_ns)),
                             0,
                             0,
                             stamp.size,
                             type,
                             llvm::sys::fs::all_all);
}

std::expected<Status, std::error_code> status(llvm::StringRef path) {
#ifdef _WIN32
    return by_path(path);
#else
    llvm::SmallString<256> terminated(path);
    struct stat info;
    if(::stat(terminated.c_str(), &info) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return make_status(info);
#endif
}

std::expected<Status, std::error_code> StatusBatch::status(llvm::StringRef path) {
#ifdef _WIN32
    auto& directory = directories[llvm::sys::path::parent_path(path)];
    if(!directory.listed) {
        directory.asked += 1;
        if(directory.asked < list_after) {
            auto status = vfs::status(path);
            // NTFS updates a directory entry's size and time only for the
            // link a write went through: a directory of hard links (Boost's
            // `b2 headers`) cannot be answered from its listing.
            if(status && status->links > 1) {
                directory.listed = true;
            }
            return status;
        }
        directory.listed = true;
        directory.entries = list_statuses(llvm::sys::path::parent_path(path));
    }
    if(auto it = directory.entries.find(llvm::sys::path::filename(path));
       it != directory.entries.end()) {
        return it->second;
    }
#endif
    return vfs::status(path);
}

llvm::ErrorOr<llvm::vfs::Status> View::status(const llvm::Twine& path) {
    llvm::SmallString<256> absolute;
    path.toVector(absolute);
    if(auto error = getUnderlyingFS().makeAbsolute(absolute)) {
        return error;
    }
    auto status = statuses.status(absolute);
    if(!status) {
        return status.error();
    }
    auto result = status->to_llvm(path.str());
    if(!status->is_file() || status->stamp.size < 3 || !starts_with_bom(status->stamp, absolute)) {
        return result;
    }
    return llvm::vfs::Status::copyWithNewSize(result, status->stamp.size - 3);
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
        auto status = statuses.status(absolute);
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

bool settled(std::int64_t mtime_ns) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return mtime_ns <= stat_baseline_before_ns(now_ms);
}

bool exists(llvm::StringRef path) {
#ifdef _WIN32
    // A status by name needs Windows 11 24H2; attributes are by name
    // everywhere.
    llvm::SmallVector<wchar_t, 256> wide;
    if(widen(path, wide)) {
        return false;
    }
    auto attributes = ::GetFileAttributesW(wide.data());
    if(attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    // Attributes describe a link itself, not what it points to.
    return !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) || status(path).has_value();
#else
    return status(path).has_value();
#endif
}

bool is_file(llvm::StringRef path) {
    auto status = vfs::status(path);
    return status && status->is_file();
}

bool is_directory(llvm::StringRef path) {
    auto status = vfs::status(path);
    return status && status->type == llvm::sys::fs::file_type::directory_file;
}

bool is_symlink(llvm::StringRef path) {
#ifdef _WIN32
    // LLVM's status reports a link as what it points to.
    llvm::SmallVector<wchar_t, 256> wide;
    if(widen(path, wide)) {
        return false;
    }
    HANDLE handle = ::CreateFileW(wide.data(),
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(handle); });
    FILE_ATTRIBUTE_TAG_INFO info;
    return ::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) &&
           is_link(info.FileAttributes, info.ReparseTag);
#else
    return llvm::sys::fs::is_symlink_file(path);
#endif
}

std::expected<std::vector<Entry>, std::error_code> read_dir(llvm::StringRef dir) {
    std::vector<Entry> entries;
#ifdef _WIN32
    // LLVM's listing reports a link as what it points to.
    auto handle = open_directory(dir);
    if(!handle) {
        return std::unexpected(handle.error());
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(*handle); });
    auto error = for_each_entry<FILE_FULL_DIR_INFO>(
        *handle,
        FileFullDirectoryRestartInfo,
        FileFullDirectoryInfo,
        [&](const FILE_FULL_DIR_INFO& entry, llvm::StringRef name) {
            llvm::SmallString<256> path(dir);
            llvm::sys::path::append(path, name);
            // A reparse point's tag stands where the size of extended
            // attributes would.
            auto type = is_link(entry.FileAttributes, entry.EaSize)
                            ? llvm::sys::fs::file_type::symlink_file
                        : entry.FileAttributes & FILE_ATTRIBUTE_DIRECTORY
                            ? llvm::sys::fs::file_type::directory_file
                            : llvm::sys::fs::file_type::regular_file;
            entries.push_back({.path = path.str().str(), .type = type});
            return true;
        });
#else
    std::error_code error;
    for(llvm::sys::fs::directory_iterator it(dir, error, /*follow_symlinks=*/false), end;
        !error && it != end;
        it.increment(error)) {
        entries.push_back({.path = it->path(), .type = it->type()});
    }
#endif
    if(error) {
        return std::unexpected(error);
    }
    return entries;
}

void walk(llvm::StringRef root, llvm::function_ref<bool(const Entry&)> visit) {
    auto entries = read_dir(root);
    if(!entries) {
        if(entries.error() != std::errc::no_such_file_or_directory) {
            LOG_WARN("Cannot read directory {}: {}", root, entries.error().message());
        }
        return;
    }
    for(auto& entry: *entries) {
        if(visit(entry) && entry.type == llvm::sys::fs::file_type::directory_file) {
            walk(entry.path, visit);
        }
    }
}

std::error_code create_directories(llvm::StringRef path) {
    return llvm::sys::fs::create_directories(path);
}

std::error_code write(llvm::StringRef path, llvm::StringRef content) {
    // Opened here rather than by the stream, which would take "-" for
    // stdout and could then not be closed.
    int fd = -1;
    if(auto error = llvm::sys::fs::openFileForWrite(path, fd)) {
        return error;
    }
    llvm::raw_fd_ostream os(fd, /*shouldClose=*/true);
    os << content;
    os.close();
    auto error = os.error();
    // An uncleared error aborts in the stream's destructor.
    os.clear_error();
    return error;
}

std::error_code write_atomic(llvm::StringRef path, llvm::StringRef content) {
    llvm::SmallString<256> sibling;
    if(auto error = llvm::sys::fs::createUniqueFile(path + ".%%%%%%", sibling)) {
        return error;
    }
    auto error = write(sibling, content);
    if(!error) {
        error = rename(sibling, path);
    }
    if(error) {
        remove(sibling);
    }
    return error;
}

std::error_code rename(llvm::StringRef from, llvm::StringRef to) {
    return llvm::sys::fs::rename(from, to);
}

std::error_code remove(llvm::StringRef path) {
#ifdef _WIN32
    llvm::SmallVector<wchar_t, 256> wide;
    if(auto error = widen(path, wide)) {
        return error;
    }
    HANDLE handle = ::CreateFileW(wide.data(),
                                  DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        auto error = llvm::mapWindowsError(::GetLastError());
        if(error == std::errc::no_such_file_or_directory) {
            return {};
        }
        return error;
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(handle); });
    FILE_DISPOSITION_INFO_EX posix{FILE_DISPOSITION_FLAG_DELETE |
                                   FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
    if(::SetFileInformationByHandle(handle, FileDispositionInfoEx, &posix, sizeof(posix))) {
        return {};
    }
    // Filesystems without POSIX deletes (FAT) refuse the flag: a mapped
    // file cannot go there at all.
    FILE_DISPOSITION_INFO classic{TRUE};
    if(::SetFileInformationByHandle(handle, FileDispositionInfo, &classic, sizeof(classic))) {
        return {};
    }
    return llvm::mapWindowsError(::GetLastError());
#else
    return llvm::sys::fs::remove(path);
#endif
}

std::error_code remove_all(llvm::StringRef path) {
    llvm::sys::fs::file_status status;
    if(auto error = llvm::sys::fs::status(path, status, /*follow=*/false)) {
        return error == std::errc::no_such_file_or_directory ? std::error_code() : error;
    }
    bool directory = status.type() == llvm::sys::fs::file_type::directory_file;
#ifdef _WIN32
    directory = directory && !is_symlink(path);
#endif
    if(!directory) {
        return remove(path);
    }
    auto entries = read_dir(path);
    if(!entries) {
        return entries.error() == std::errc::no_such_file_or_directory ? std::error_code()
                                                                       : entries.error();
    }
    for(auto& entry: *entries) {
        auto error = entry.type == llvm::sys::fs::file_type::directory_file ? remove_all(entry.path)
                                                                            : remove(entry.path);
        if(error) {
            return error;
        }
    }
    return remove(path);
}

std::expected<std::string, std::error_code> temp_file(llvm::StringRef prefix,
                                                      llvm::StringRef suffix) {
    llvm::SmallString<128> path;
    if(auto error = llvm::sys::fs::createTemporaryFile(prefix, suffix, path)) {
        return std::unexpected(error);
    }
    return path.str().str();
}

}  // namespace clice::vfs
