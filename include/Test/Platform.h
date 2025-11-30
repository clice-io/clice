namespace clice::testing {

/// True if the target platform is Windows.
#ifdef _WIN32
constexpr inline bool Windows = true;
#else
constexpr inline bool Windows = false;
#endif

/// True if the target platform is macOS or other Apple OS.
#ifdef __APPLE__
constexpr inline bool MacOS = true;
#else
constexpr inline bool MacOS = false;
#endif

/// True if the target platform is Linux. Note: This may also be true on Android.
#ifdef __linux__
constexpr inline bool Linux = true;
#else
constexpr inline bool Linux = false;
#endif

/// True if the compiler is Clang.
#if defined(__clang__)
constexpr inline bool Clang = true;
#else
constexpr inline bool Clang = false;
#endif

/// True if the compiler is GCC.
#if defined(__GNUC__) && !defined(__clang__)
constexpr inline bool GCC = true;
#else
constexpr inline bool GCC = false;
#endif

/// True if the compiler is MSVC.
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

}  // namespace clice::testing
