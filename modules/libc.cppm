module;

#if defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)
#include "linux-x64/libc.fragment.h"
#elif defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)
#include "linux-x64-debug/libc.fragment.h"
#elif defined(__linux__) && defined(__aarch64__) && defined(NDEBUG)
#include "linux-arm64/libc.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)
#include "macos-arm64/libc.fragment.h"
#elif defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
#include "macos-arm64-debug/libc.fragment.h"
#elif defined(__APPLE__) && defined(__x86_64__) && defined(NDEBUG)
#include "macos-x64/libc.fragment.h"
#elif defined(_WIN32) && defined(__x86_64__) && defined(NDEBUG)
#include "windows-x64/libc.fragment.h"
#elif defined(_WIN32) && defined(__aarch64__) && defined(NDEBUG)
#include "windows-arm64/libc.fragment.h"
#else
#error "no configuration merged matches this compilation"
#endif

export module libc;

export using ::abs;
export using ::fileno;
export using ::strcasecmp;

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)) || (defined(__linux__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)) || (defined(__APPLE__) && defined(__x86_64__) && defined(NDEBUG))
export using ::kill;
export using ::localtime_r;
export using ::off_t;
export using ::pid_t;
export using ::pthread_t;
export using ::putenv;
export using ::setenv;
export using ::ssize_t;
#endif

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)) || (defined(__linux__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(_WIN32) && defined(__x86_64__) && defined(NDEBUG)) || (defined(_WIN32) && defined(__aarch64__) && defined(NDEBUG))
export using ::strncasecmp;
#endif

#if (defined(__linux__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)) || (defined(_WIN32) && defined(__aarch64__) && defined(NDEBUG))
export using ::int16x8_t;
export using ::int64x1_t;
export using ::int64x2_t;
export using ::int8x16_t;
export using ::int8x8_t;
export using ::uint16x8_t;
export using ::uint64x1_t;
export using ::uint64x2_t;
export using ::uint8x16_t;
export using ::uint8x8_t;
#endif

#if (defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)) || (defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)) || (defined(__linux__) && defined(__aarch64__) && defined(NDEBUG))
export using ::__errno_location;
export using ::__sighandler_t;
export using ::stderr;
export using ::stdout;
#endif

#if (defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)) || (defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)) || (defined(__APPLE__) && defined(__x86_64__) && defined(NDEBUG))
export using ::__error;
export using ::__stderrp;
export using ::__stdoutp;
#endif

#if (defined(_WIN32) && defined(__x86_64__) && defined(NDEBUG)) || (defined(_WIN32) && defined(__aarch64__) && defined(NDEBUG))
export using ::__acrt_iob_func;
export using ::__p_sig_fn_t;
export using ::_errno;
export using ::_putenv_s;
export using ::_wfopen;
export using ::localtime_s;
export using ::mode_t;
#endif

#if defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)
export using ::__assert_fail;
#endif

#if defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
export using ::__assert_rtn;
#endif
