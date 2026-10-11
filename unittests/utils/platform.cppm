module;

#include "modules/prelude.h"

module clice:unittests.utils.platform;

import :vfs.path;

namespace clice::testing {

#ifdef _WIN32
constexpr inline bool Windows = true;
#else
constexpr inline bool Windows = false;
#endif

#ifdef __linux__
constexpr inline bool Linux = true;
#else
constexpr inline bool Linux = false;
#endif

class TestVFS : public llvm::vfs::InMemoryFileSystem {
public:
    TestVFS() {
        setCurrentWorkingDirectory(root());
    }

    const static char* root() {
#ifdef _WIN32
        return "c:/clice-test";
#else
        return "/clice-test";
#endif
    }

    /// root() + relative → absolute path, spelled the way the file table
    /// names a file (path::canonicalize); an absolute path stays as is (a
    /// file that must live outside the root, e.g. inside a cache store's
    /// directory).
    static std::string path(llvm::StringRef relative) {
        if(llvm::sys::path::is_absolute(relative)) {
            return relative.str();
        }
        llvm::SmallString<128> joined;
        llvm::sys::path::append(joined, root(), relative);
        std::string result(joined);
        path::canonicalize(result);
        return result;
    }

    /// Add a file with an optional content (relative path, auto-prefixed
    /// with root(), or absolute).
    void add(llvm::StringRef relative, llvm::StringRef content = {}) {
        auto p = path(relative);
        addFile(p, 0, llvm::MemoryBuffer::getMemBufferCopy(content, p));
    }
};

}  // namespace clice::testing
