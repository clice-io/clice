#ifdef PLATFORM_WINDOWS
int open_handle(const char* path) {
    if (path == nullptr) {
        return -1;
    }
    return 0;
}

struct Handle {
    void* raw;
    int flags;
};
#else
int open_handle(const char* path) {
    return path == nullptr ? -1 : 0;
}
#endif
