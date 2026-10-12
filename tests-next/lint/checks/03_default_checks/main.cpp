// Pins the checks that run besides those a .clang-tidy names: without one,
// the editor's default set; with one, the compiler warnings of the build too,
// as clang-tidy reports them (its checks start from clang-diagnostic-*).
double ratio(int done, int total) {
    return done / total;
}

int* none() {
    int unused;
    return 0;
}
