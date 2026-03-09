#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice::testing {

#ifdef _WIN32
constexpr inline bool Windows = true;
#else
constexpr inline bool Windows = false;
#endif

#ifdef __APPLE__
constexpr inline bool macOS = true;
#else
constexpr inline bool macOS = false;
#endif

#ifdef __linux__
constexpr inline bool Linux = true;
#else
constexpr inline bool Linux = false;
#endif

#if defined(__clang__)
constexpr inline bool Clang = true;
#else
constexpr inline bool Clang = false;
#endif

#if defined(__GNUC__) && !defined(__clang__)
constexpr inline bool GCC = true;
#else
constexpr inline bool GCC = false;
#endif

#if defined(_MSC_VER) && !defined(__clang__)
constexpr inline bool MSVC = true;
#else
constexpr inline bool MSVC = false;
#endif

#ifdef CLICE_CI_ENVIRONMENT
constexpr inline bool CIEnvironment = true;
#else
constexpr inline bool CIEnvironment = false;
#endif

class TestVFS : public llvm::vfs::InMemoryFileSystem {
public:
    TestVFS() {
        setCurrentWorkingDirectory(root());
    }

    const static char* root() {
#ifdef _WIN32
        return "C:\\clice-test";
#else
        return "/clice-test";
#endif
    }

    /// root() + relative → absolute path.
    static std::string path(llvm::StringRef relative) {
        llvm::SmallString<128> result;
        llvm::sys::path::append(result, root(), relative);
        return std::string(result);
    }

    /// Add a file with an optional content (relative path, auto-prefixed with root()).
    void add(llvm::StringRef relative, llvm::StringRef content = {}) {
        auto p = path(relative);
        addFile(p, 0, llvm::MemoryBuffer::getMemBufferCopy(content, p));
    }
};

}  // namespace clice::testing
