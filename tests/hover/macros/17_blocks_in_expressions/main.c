#define MAX(a, b)                                                                                \
    ({                                                                                           \
        __typeof__(a) _a = (a);                                                                  \
        __typeof__(b) _b = (b);                                                                  \
        _a > _b ? _a : _b;                                                                       \
    })

#define CHECKED(value, limit)                                                                    \
    ((int)sizeof(struct { _Static_assert((limit) > 0, "limit must be positive"); }) * 0 + (value))

int larger(int x, int y) {
    return §(max)MAX(x, y);
}

int bounded(int x) {
    return §(checked)CHECKED(x, 16);
}
