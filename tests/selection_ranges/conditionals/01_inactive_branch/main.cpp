// A cursor inside a branch the preprocessor skipped starts from what
// encloses the branch.

int pick(int value) {
#if 0
    return §(skipped)value + 1;
#endif
    return value;
}
