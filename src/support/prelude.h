// Macro prelude — force-included into every first-party translation unit via
// `-include` (see CMakeLists.txt, clice_options). It carries the logging,
// anomaly, and assertion macros so their expansions reach every TU without a
// textual header. Macros cannot cross a module import boundary, so this is the
// only way module units can use them; the names they reference
// (clice::logging::*, std::*) are resolved at the EXPANSION site, which imports
// the partitions and stdlib.
//
// ZERO-TOKEN LAW: this file must contain preprocessor directives ONLY — no
// #include, no declaration, not even a stray semicolon at file scope. It is
// injected ahead of the `module;` / `export module` declaration in module
// units, where ANY token before the module declaration (a declaration, or an
// #include that pulls one in) is ill-formed ("module declaration must occur at
// the start of the translation unit"). Never let this header gain an include
// or a declaration.
//
// MSVC note: a clang-cl / MSVC frontend would force-include with `/FI` instead
// of `-include` (CMakeLists switches on the frontend variant); every current CI
// toolchain is GNU-driver clang.

#ifndef CLICE_PRELUDE_H
#define CLICE_PRELUDE_H

#define LOG_MESSAGE(name, fmt, ...)                                                                \
    do {                                                                                           \
        if(clice::logging::options.level <= clice::logging::Level::name) {                         \
            clice::logging::name(fmt __VA_OPT__(, ) __VA_ARGS__);                                  \
        }                                                                                          \
    } while(0)

#define LOG_TRACE(fmt, ...) LOG_MESSAGE(trace, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_MESSAGE(debug, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MESSAGE(info, fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MESSAGE(warn, fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_MESSAGE(err, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...) clice::logging::critical(fmt __VA_OPT__(, ) __VA_ARGS__);

/// Performance measurement line (see the taxonomy in support/logging.cppm):
/// `topic` must be a string literal, the message should be stable key=value
/// pairs.
#define LOG_PERF(topic, fmt, ...) LOG_INFO("[perf:" topic "] " fmt __VA_OPT__(, ) __VA_ARGS__)

#define LOG_MESSAGE_RET(ret, name, fmt, ...)                                                       \
    do {                                                                                           \
        LOG_MESSAGE(name, fmt, __VA_ARGS__);                                                       \
        return ret;                                                                                \
    } while(0);

#define LOG_TRACE_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, trace, fmt, __VA_ARGS__)
#define LOG_DEBUG_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, debug, fmt, __VA_ARGS__)
#define LOG_INFO_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, info, fmt, __VA_ARGS__)
#define LOG_WARN_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, warn, fmt, __VA_ARGS__)
#define LOG_ERROR_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, err, fmt, __VA_ARGS__)

/// Soft assertion for internal invariants (see AnomalyId in
/// support/anomaly.cppm). The gate runs before the format arguments are
/// evaluated, so suppressed reports cost nothing — arguments must not carry
/// needed side effects.
#define LOG_ANOMALY(id, fmt, ...)                                                                  \
    do {                                                                                           \
        if(clice::logging::anomaly_should_report(clice::logging::AnomalyId::id)) {                 \
            clice::logging::report_anomaly(clice::logging::AnomalyId::id,                          \
                                           std::format(fmt __VA_OPT__(, ) __VA_ARGS__));           \
        }                                                                                          \
    } while(0)

/// User-facing guidance via window/logMessage (master) and the log file.
/// Same lazy-evaluation contract as LOG_ANOMALY.
#define LOG_GUIDANCE(fmt, ...)                                                                     \
    do {                                                                                           \
        if(clice::logging::guidance_should_report()) {                                             \
            clice::logging::report_guidance(std::format(fmt __VA_OPT__(, ) __VA_ARGS__));          \
        }                                                                                          \
    } while(0)

/// Assertion that routes through the logging transport (crash trace + flushed
/// log) instead of libc's assert, so a failure is captured in the process log
/// like LOG_FATAL. Compiled out under NDEBUG.
#ifdef NDEBUG
#define CLICE_ASSERT(...) ((void)0)
#else
#define CLICE_ASSERT(...)                                                                          \
    ((__VA_ARGS__)                                                                                 \
         ? (void)0                                                                                 \
         : ::clice::logging::assert_fail(#__VA_ARGS__, ::std::source_location::current()))
#endif

#endif  // CLICE_PRELUDE_H
