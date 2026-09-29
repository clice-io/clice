// The Windows executable allocates through mimalloc: the UCRT heap returns
// freed memory to the system between requests, so every request faulted
// its working set back in page by page. Linked into the executable itself
// so these definitions win over the UCRT imports for all statically linked
// code (clice, LLVM, libc++). Blocks the UCRT allocated for itself
// (`strdup`, `_wgetcwd(nullptr)`, ...) still reach free() and realloc();
// they go back to the UCRT.

#include <cstddef>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mimalloc.h"

namespace {

template <typename F>
F ucrt(const char* name) {
    static HMODULE module = ::GetModuleHandleW(L"ucrtbase.dll");
    return reinterpret_cast<F>(::GetProcAddress(module, name));
}

void ucrt_free(void* p) {
    static auto fn = ucrt<void (*)(void*)>("free");
    fn(p);
}

std::size_t ucrt_msize(void* p) {
    static auto fn = ucrt<std::size_t (*)(void*)>("_msize");
    return fn(p);
}

/// Keep freed memory in the process, as glibc does: purging it made the
/// next request fault it back in.
const bool retain = [] {
    mi_option_set(mi_option_purge_delay, -1);
    return true;
}();

}  // namespace

extern "C" {

    void* malloc(std::size_t size) {
        return mi_malloc(size);
    }

    void* calloc(std::size_t count, std::size_t size) {
        return mi_calloc(count, size);
    }

    void free(void* p) {
        if(!p) {
            return;
        }
        if(mi_is_in_heap_region(p)) {
            mi_free(p);
        } else {
            ucrt_free(p);
        }
    }

    void* realloc(void* p, std::size_t size) {
        if(!p || mi_is_in_heap_region(p)) {
            return mi_realloc(p, size);
        }
        // A block the UCRT allocated moves to mimalloc.
        auto old = ucrt_msize(p);
        auto fresh = mi_malloc(size);
        if(fresh) {
            std::memcpy(fresh, p, old < size ? old : size);
            ucrt_free(p);
        }
        return fresh;
    }

}  // extern "C"
