#define SWAP(a, b)                                                                                 \
    do {                                                                                           \
        int *t = a;                                                                                \
        a = b;                                                                                     \
        b = t;                                                                                     \
    } while(0)

int *§(declaration)find(int *first, int *last, int value);

void exchange(int *x, int *y) {
    §(expansion)SWAP(x, y);
}
