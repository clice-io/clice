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

}  // namespace clice::testing
