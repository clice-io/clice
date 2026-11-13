// Pins that a suppression comment silences a compiler warning when the
// checks enable no clang-tidy check of their own, only warnings.
int scale(int value) {
    int reported;
    int silenced;  // NOLINT
    return value;
}
