namespace {

struct Cache {};

int cache_size() {
    return 0;
}

}  // namespace

static int cache_hits = 0;

int use() {
    int cache_local = cache_hits;
    return cache_local + cache_size();
}
