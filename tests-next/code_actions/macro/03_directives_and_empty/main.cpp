// A macro named in a preprocessor condition, on any of its lines, is no
// expansion to replace, while a macro expanding to nothing is deleted.

#define GUARD 1
#define EMPTY

#if §(directive)GUARD
int guarded = §(empty)EMPTY 1;
#endif

#if GUARD && \
    §(continued)GUARD
int continued;
#endif
