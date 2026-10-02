load("@rules_cc//cc:cc_library.bzl", "cc_library")

# From the BCR overlay of libuv 1.48.0.bcr.2, for 1.52.0 (same sources) and
# GNU-style flags on Windows too (MinGW clang, not cl.exe).
_LOCAL_DEFINES = select({
    "@platforms//os:windows": [
        "WIN32_LEAN_AND_MEAN",
        "_WIN32_WINNT=0x0A00",
        "_CRT_DECLARE_NONSTDC_NAMES=0",
    ],
    "//conditions:default": [
        "_FILE_OFFSET_BITS=64",
        "_LARGEFILE_SOURCE",
    ],
}) + select({
    "@platforms//os:macos": [
        "_DARWIN_UNLIMITED_SELECT=1",
        "_DARWIN_USE_64_BIT_INODE=1",
    ],
    "@platforms//os:linux": [
        "_GNU_SOURCE",
        "_POSIX_C_SOURCE=200112",
    ],
    "//conditions:default": [],
})

cc_library(
    name = "src_headers",
    hdrs = glob(["src/**/*.h"]),
    strip_include_prefix = "src",
)

cc_library(
    name = "libuv",
    srcs = [
        "src/fs-poll.c",
        "src/idna.c",
        "src/inet.c",
        "src/random.c",
        "src/strscpy.c",
        "src/strtok.c",
        "src/thread-common.c",
        "src/threadpool.c",
        "src/timer.c",
        "src/uv-common.c",
        "src/uv-data-getter-setters.c",
        "src/version.c",
    ] + select({
        "@platforms//os:windows": glob(["src/win/*.c"]),
        "//conditions:default": [
            "src/unix/async.c",
            "src/unix/core.c",
            "src/unix/dl.c",
            "src/unix/fs.c",
            "src/unix/getaddrinfo.c",
            "src/unix/getnameinfo.c",
            "src/unix/loop.c",
            "src/unix/loop-watcher.c",
            "src/unix/pipe.c",
            "src/unix/poll.c",
            "src/unix/process.c",
            "src/unix/proctitle.c",
            "src/unix/random-devurandom.c",
            "src/unix/signal.c",
            "src/unix/stream.c",
            "src/unix/tcp.c",
            "src/unix/thread.c",
            "src/unix/tty.c",
            "src/unix/udp.c",
        ],
    }) + select({
        "@platforms//os:macos": [
            "src/unix/bsd-ifaddrs.c",
            "src/unix/darwin.c",
            "src/unix/darwin-proctitle.c",
            "src/unix/fsevents.c",
            "src/unix/kqueue.c",
            "src/unix/random-getentropy.c",
        ],
        "@platforms//os:linux": [
            "src/unix/linux.c",
            "src/unix/procfs-exepath.c",
            "src/unix/random-getrandom.c",
            "src/unix/random-sysctl-linux.c",
        ],
        "//conditions:default": [],
    }),
    hdrs = glob(["include/**/*.h"]),
    copts = ["-w", "-fno-strict-aliasing", "-fvisibility=hidden"],
    linkopts = select({
        "@platforms//os:windows": ["-lpsapi", "-luser32", "-ladvapi32", "-liphlpapi", "-luserenv", "-lws2_32", "-ldbghelp", "-lole32", "-lshell32"],
        "@platforms//os:linux": ["-pthread", "-ldl", "-lrt"],
        "//conditions:default": ["-pthread"],
    }),
    local_defines = _LOCAL_DEFINES,
    strip_include_prefix = "include",
    visibility = ["//visibility:public"],
    deps = [":src_headers"],
)
