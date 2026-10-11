// Pins the checks that run besides those a .clang-tidy names: without one,
// the editor's default set; with one, clang-tidy would also report the
// compiler warnings of the build (its checks start from clang-diagnostic-*).
double ratio(int done, int total) {
    return done / total;
}

int* none() {
    int unused;
    return 0;
}
