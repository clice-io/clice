#define CHECK(x) ((void)(x))
#define MAKE_STRUCT(name, body) struct name body;

int compute(int a, int b, int c);

void verify() {
    CHECK(compute(
        4,
        5,
        6));

    CHECK([] {
        int q = 1;
        return q;
    }());
}

MAKE_STRUCT(Generated, {
    int a;
    int b;
})
