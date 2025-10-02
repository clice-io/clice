
namespace clice::testing {

/// True if the target platform is Windows.
#ifdef _WIN32
constexpr inline bool windows = true;
#else
constexpr inline bool windows = false;
#endif

/// True if the target platform is macOS or other Apple OS.
#ifdef __APPLE__
constexpr inline bool macos = true;
#else
constexpr inline bool macos = false;
#endif

/// True if the target platform is Linux. Note: This may also be true on Android.
#ifdef __linux__
constexpr inline bool linux = true;
#else
constexpr inline bool linux = false;
#endif

/// True if the compiler is Clang.
#if defined(__clang__)
constexpr inline bool clang = true;
#else
constexpr inline bool clang = false;
#endif

/// True if the compiler is GCC.
#if defined(__GNUC__) && !defined(__clang__)
constexpr inline bool gcc = true;
#else
constexpr inline bool gcc = false;
#endif

/// True if the compiler is MSVC.
#if defined(_MSC_VER) && !defined(__clang__)
constexpr inline bool msvc = true;
#else
constexpr inline bool msvc = false;
#endif

#ifdef CLICE_CI_ENVIRONMENT
constexpr inline bool ci_environment = true;
#else
constexpr inline bool ci_environment = false;
#endif
}  // namespace clice::testing
