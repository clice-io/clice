// Pins --configuration: the run checks the units under the commands of the
// configuration it names, the default one without it.
#ifdef RELEASE
double release_ratio(int done, int total) {
    return done / total;
}
#else
double debug_ratio(int done, int total) {
    return done / total;
}
#endif
