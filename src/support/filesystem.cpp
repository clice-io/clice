#include "support/filesystem.h"

#include <mutex>
#include <optional>
#include <tuple>

#include "llvm/ADT/DenseMap.h"

#ifdef _WIN32
#include <windows.h>

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/ConvertUTF.h"
#endif

namespace clice {

namespace {

#ifdef _WIN32

/// The final name the OS gives what `path` opens: on-disk case, 8.3 names
/// expanded, junctions, symlinks and subst drives followed. The handle asks
/// for no data access, so neither a sharing mode nor a missing read
/// permission refuses it.
std::optional<std::string> resolve_existing(llvm::StringRef path) {
    std::wstring wide;
    if(!llvm::ConvertUTF8toWide(path, wide)) {
        return std::nullopt;
    }
    HANDLE handle = ::CreateFileW(wide.c_str(),
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if(handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    auto close = llvm::make_scope_exit([&] { ::CloseHandle(handle); });

    std::wstring final(MAX_PATH, L'\0');
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    auto count = ::GetFinalPathNameByHandleW(handle, final.data(), final.size(), flags);
    if(count >= final.size()) {
        // Too small: the count then includes the terminator.
        final.resize(count);
        count = ::GetFinalPathNameByHandleW(handle, final.data(), final.size(), flags);
    }
    if(count == 0 || count >= final.size()) {
        return std::nullopt;
    }
    final.resize(count);

    std::wstring_view name = final;
    if(name.starts_with(LR"(\\?\UNC\)")) {
        final = LR"(\\)" + final.substr(8);
    } else if(name.starts_with(LR"(\\?\)") && name.size() >= 6 && name[5] == L':') {
        final = final.substr(4);
    }
    std::string result;
    if(!llvm::convertWideToUTF8(final, result)) {
        return std::nullopt;
    }
    return result;
}

/// `.`, `..`, separators, trailing dots and spaces removed the way Win32
/// does before it opens anything.
std::string lexical(llvm::StringRef spelled) {
    std::wstring wide;
    if(!llvm::ConvertUTF8toWide(spelled, wide)) {
        return spelled.str();
    }
    std::wstring full(MAX_PATH, L'\0');
    auto count = ::GetFullPathNameW(wide.c_str(), full.size(), full.data(), nullptr);
    if(count >= full.size()) {
        full.resize(count);
        count = ::GetFullPathNameW(wide.c_str(), full.size(), full.data(), nullptr);
    }
    std::string result;
    if(count == 0 || count >= full.size() ||
       !llvm::convertWideToUTF8(full.substr(0, count), result)) {
        return spelled.str();
    }
    return result;
}

#else

std::optional<std::string> resolve_existing(llvm::StringRef path) {
    llvm::SmallString<256> real;
    if(llvm::sys::fs::real_path(path, real)) {
        return std::nullopt;
    }
    return std::string(real);
}

/// `..` stays: the OS resolves it physically, past symlinks.
std::string lexical(llvm::StringRef spelled) {
    return spelled.str();
}

#endif

bool read_starts_with_bom(llvm::StringRef path) {
    auto fd = llvm::sys::fs::openNativeFileForRead(path);
    if(!fd) {
        llvm::consumeError(fd.takeError());
        return false;
    }
    char head[3];
    auto read = llvm::sys::fs::readNativeFile(*fd, head);
    llvm::sys::fs::closeFile(*fd);
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
bool starts_with_bom(const vfs::Status& status, llvm::StringRef path) {
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

/// `.` segments, duplicate and trailing separators dropped, canonically
/// spelled; `..` kept.
std::string normalized(llvm::StringRef absolute) {
    llvm::SmallString<256> text(absolute);
    path::remove_dots(text, /*remove_dot_dot=*/false);
    std::string result(text);
    path::canonicalize(result);
    return result;
}

}  // namespace

llvm::StringRef path::portable(llvm::StringRef p,
                               llvm::StringRef workspace,
                               llvm::SmallVectorImpl<char>& storage) {
    if(workspace.empty() || !under(p, workspace)) {
        return p;
    }
    auto rest = p.drop_front(workspace.size());
    storage.assign(workspace_anchor.begin(), workspace_anchor.end());
    if(!rest.empty() && !is_separator(rest.front())) {
        storage.push_back('/');
    }
    storage.append(rest.begin(), rest.end());
    return llvm::StringRef(storage.data(), storage.size());
}

llvm::StringRef path::local(llvm::StringRef name,
                            llvm::StringRef workspace,
                            llvm::SmallVectorImpl<char>& storage) {
    if(!name.consume_front(workspace_anchor)) {
        return name;
    }
    assert(!workspace.empty() && "a portable name read without its workspace");
    storage.assign(workspace.begin(), workspace.end());
    if(workspace.ends_with("/")) {
        name.consume_front("/");
    }
    storage.append(name.begin(), name.end());
    return llvm::StringRef(storage.data(), storage.size());
}

Spelling::Spelling(llvm::StringRef text, const Spelling& base) {
    if(path::is_absolute(text)) {
        this->text = normalized(text);
        return;
    }
    llvm::SmallString<256> joined(base.text);
    if(path::is_rooted(text)) {
        // Windows resolves a rooted `\x` on the current drive, the base's.
        joined = path::root_name(base.text);
        joined += text;
    } else {
        path::append(joined, text);
    }
    this->text = normalized(joined);
}

Spelling Spelling::absolute(llvm::StringRef text) {
    assert(path::is_absolute(text) && "an unanchored path");
    Spelling result;
    result.text = normalized(text);
    return result;
}

Spelling Spelling::cwd() {
    llvm::SmallString<256> directory;
    llvm::sys::fs::current_path(directory);
    return absolute(directory);
}

Spelling Spelling::from_portable(llvm::StringRef name, CanonicalRef workspace) {
    llvm::SmallString<256> storage;
    return absolute(path::local(name, workspace, storage));
}

Spelling::Spelling(CanonicalRef identity) : text(identity.str()) {}

Spelling Spelling::parent() const {
    Spelling result;
    result.text = path::parent_path(text).str();
    return result;
}

llvm::ErrorOr<vfs::Status> ThreadSafeFS::status(const llvm::Twine& path) {
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
    return vfs::Status::copyWithNewSize(*status, status->getSize() - 3);
}

CanonicalPath::CanonicalPath(const Spelling& spelled) {
    auto full = lexical(spelled);
    llvm::StringRef existing = full;
    auto real = resolve_existing(existing);
    while(!real) {
        auto parent = path::parent_path(existing);
        if(parent.empty() || parent.size() == existing.size()) {
            text = std::move(full);
            path::canonicalize(text);
            return;
        }
        existing = parent;
        real = resolve_existing(existing);
    }
    llvm::SmallString<256> joined(*real);
    joined += llvm::StringRef(full).drop_front(existing.size());
    // The unresolved tail may still climb (`missing/../cache`).
    path::remove_dots(joined, /*remove_dot_dot=*/true);
    text = std::string(joined);
    path::canonicalize(text);
}

}  // namespace clice
