#include "support/filesystem.h"

#include <optional>

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

Spelling::Spelling(llvm::StringRef text, const Spelling& base) {
    if(path::is_absolute(text)) {
        this->text = normalized(text);
        return;
    }
    llvm::SmallString<256> joined(base.text);
    path::append(joined, text);
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

Spelling::Spelling(CanonicalRef identity) : text(identity.str()) {}

Spelling Spelling::parent() const {
    Spelling result;
    result.text = path::parent_path(text).str();
    return result;
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
