[[deprecated("use fresh")]] int stale();
int fresh();

int use() {
    int unused = 0;
    return stale();
}
