// A macro read on a directive's continuation line is read by the directive.
#if defined(EARLY) || \
    §(continued)LATER > 0
int positive = 1;
#endif

#define LATER 8
