// Conditional branches that each open a brace: the closing brace pairs
// with the brace of the branch the preprocessor took, the skipped one's
// pairs with nothing.

#define FAST 1

#if FAST
int fast(int value) {
#else
int slow(int value) {
#endif
    return §(inside)value * 2;
}
