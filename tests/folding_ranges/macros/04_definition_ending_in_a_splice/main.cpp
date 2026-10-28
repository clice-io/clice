#define COLORS(X) \
    X(red)        \
    X(green)      \

enum Color {
#define AS_ENUMERATOR(name) name,
    COLORS(AS_ENUMERATOR)
#undef AS_ENUMERATOR
};
