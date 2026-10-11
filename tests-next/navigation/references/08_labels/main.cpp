int loop(int failed) {
    §(label)retry:
    if (failed) {
        goto retry;
    }
    return 0;
}
