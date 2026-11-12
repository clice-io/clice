#if defined(DEBUG)
constexpr int level = 2;
#elif defined(RELEASE)
constexpr int level = 0;
#endif

int log_level() {
    return level;
}
