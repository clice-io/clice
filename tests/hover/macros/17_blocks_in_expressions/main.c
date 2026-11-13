#define MAX(a, b)                                                                                \
    ({                                                                                           \
        __typeof__(a) _a = (a);                                                                  \
        __typeof__(b) _b = (b);                                                                  \
        _a > _b ? _a : _b;                                                                       \
    })

#define CHECKED(value, limit)                                                                    \
    ((int)sizeof(struct { _Static_assert((limit) > 0, "limit must be positive"); }) * 0 + (value))

#define FIELD_SHIFT(mask) (__builtin_ctz(mask))
#define FIELD_GET(mask, reg) ((__typeof__(mask))(((reg) & (mask)) >> FIELD_SHIFT(mask)))
#define REG_FIELD_GET(reg) FIELD_GET(0x0cu, FIELD_GET(0x3cu, FIELD_GET(0xfcu, reg)))
#define SPLIT_FIELDS(out, reg)                                                                   \
    do {                                                                                         \
        (out)[0] = REG_FIELD_GET(reg);                                                           \
        (out)[1] = FIELD_GET(0x03u, reg);                                                        \
    } while(0)

int larger(int x, int y) {
    return §(max)MAX(x, y);
}

int bounded(int x) {
    return §(checked)CHECKED(x, 16);
}

void split(unsigned* out, unsigned reg) {
    §(deep)SPLIT_FIELDS(out, reg);
}
