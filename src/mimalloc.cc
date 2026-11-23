// The executables allocate through mimalloc.
//
// Linux: clang runs faster on it than on glibc's malloc, for a little more
// memory: on abseil a file opens to its diagnostics in 0.83 to 0.87 of the
// time. It takes malloc over (--@mimalloc//:override), so the C library's
// malloc, glibc's or musl's, serves nothing (release_free_memory).
//
// Windows: the UCRT heap returns freed memory to the system between
// requests, so every request faulted its working set back in page by page.
// Linked into the executable itself so these definitions win over the UCRT
// imports for all statically linked code (clice, LLVM, libc++). Blocks the
// UCRT allocated for itself (`strdup`, `_wgetcwd(nullptr)`, ...) still reach
// free() and realloc(); they go back to the UCRT.
//
// C++ allocation is routed as well: libc++ implements the aligned
// `operator new` with the UCRT's `_aligned_malloc`, an import no
// definition here can replace, and LLVM allocates its hash tables and
// bump allocator slabs through it (llvm::allocate_buffer).
//
// macOS: libmalloc hands the pages it took back to later allocations
// without telling the kernel, whose footprint then leaves them out: a
// stateless worker measured a fraction of what it used, and the worker
// scheduler sizes background work by that footprint. mimalloc tells the
// kernel when it reuses a page. It takes malloc and operator new over
// itself (--@mimalloc//:override), its malloc zone the system libraries'
// allocations too.
//
// FIXME: on Windows two of mimalloc's process-attach hooks, its constructor
// and its TLS callback, both run its start-up, and every process starts with
// a stray newline on stderr, which keeps an editor's stderr pipe from
// switching to non-blocking until the editor reads it (logging::PipeOutput).
// Fix it in the registry's mimalloc: its start-up runs once.

#ifdef _WIN32
#include <cstddef>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "mimalloc-new-delete.h"
#endif

#include "mimalloc.h"

namespace {

#ifdef _WIN32

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

/// Return freed memory to the system a second after it is freed: what a
/// request frees and the next one reuses stays, and a collect returns the
/// rest. A negative delay would schedule nothing for purging, and no collect
/// would return it: every process would stay at its high-water mark.
const bool purge = [] {
    mi_option_set(mi_option_purge_delay, 1000);
    return true;
}();

#endif

/// A segment a thread left when it ended goes to the thread that frees into
/// it, whose collects return it: otherwise it waits for an allocation of its
/// size, and an idle process keeps it for good.
const bool reclaim = [] {
    mi_option_set(mi_option_abandoned_reclaim_on_free, 1);
    return true;
}();

void collect() {
    mi_collect(true);
}

}  // namespace

extern "C" {

    extern void (*clice_allocator_collect)();
}

namespace {

const bool hooked = [] {
    clice_allocator_collect = &collect;
    return true;
}();

}  // namespace

#ifdef _WIN32

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

#endif
