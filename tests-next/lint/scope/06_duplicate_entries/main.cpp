// Pins a file the database lists twice, under different commands, as a build
// compiling it for two targets does: each command makes a translation unit
// of its own, with a finding of its own.
#ifdef FAST
double fast_ratio(int done, int total) {
    return done / total;
}
#else
double slow_ratio(int done, int total) {
    return done / total;
}
#endif
