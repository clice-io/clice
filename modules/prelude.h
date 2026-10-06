#pragma once

#if defined(__linux__) && defined(__x86_64__) && defined(NDEBUG)
#include "linux-x64/prelude.h"
#elif defined(__linux__) && defined(__x86_64__) && !defined(NDEBUG)
#include "linux-x64-debug/prelude.h"
#elif defined(__linux__) && defined(__aarch64__)
#include "linux-arm64/prelude.h"
#elif defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG)
#include "macos-arm64/prelude.h"
#elif defined(__APPLE__) && defined(__aarch64__) && !defined(NDEBUG)
#include "macos-arm64-debug/prelude.h"
#elif defined(__APPLE__) && defined(__x86_64__)
#include "macos-x64/prelude.h"
#elif defined(_WIN32) && defined(__x86_64__)
#include "windows-x64/prelude.h"
#elif defined(_WIN32) && defined(__aarch64__)
#include "windows-arm64/prelude.h"
#else
#error "no configuration merged matches this compilation"
#endif
