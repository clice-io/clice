#define CHECK(condition) ((void)(condition))
#define TABLE(...) int table[] = {__VA_ARGS__}

int compute(int a, int b);

using Callback = void (*)(
    int code,
    const char* message);

[[deprecated,
  nodiscard]] int legacy();

int run(int a, int b) {
    if (a > 0 &&
        b > 0 &&
        a != b) {
        a = 0;
    }

    for (int i = 0;
         i < a;
         i += 1) {
        b += i;
    }

    int values[4] = {1, 2, 3, 4};
    b += values[
        a % 4
    ];

    auto value = (a +
                  b +
                  compute(a, b));

    auto widened = static_cast<long>(
        a * b);

    static_assert(sizeof(int) >=
                      2,
                  "int too small");

    CHECK(
        a ==
        b);

    TABLE(
        1,
        2,
        3);

    return value + static_cast<int>(widened) + table[0];
}
