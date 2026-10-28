#define FAST 1

int pick(int value) {
#if FAST
    value *= 2;
    return §(active)value;
#else
    return §(skipped)value + 1;
#endif
}

#ifdef §(directive)FAST
int fast_enabled = 1;
#endif
