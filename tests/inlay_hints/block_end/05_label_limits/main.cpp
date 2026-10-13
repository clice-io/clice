// Each kind of hint text has its own length limit.
void configure(int retries, const char* name = "primary", bool secure = false);

namespace storage_layer {
#if defined(ENABLE_CACHE) && CACHE_SIZE > 4
int cached();
#endif

int count(int total) {
    while (total > 0) {
        total -= 1;
    }
    configure(total);
    return total;
}

}
