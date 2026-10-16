// Pins the checks that run besides those a .clang-tidy names, as clang-tidy
// runs them: its checks start from clang-diagnostic-*, the compiler warnings
// of the build, which are all that run without one.
double ratio(int done, int total) {
    return done / total;
}

int* none() {
    int unused;
    return 0;
}
