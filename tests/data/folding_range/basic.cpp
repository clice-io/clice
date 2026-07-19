// Seed case for the per-feature snapshot corpus: basic folding kinds.
namespace demo {

struct Point {
    int x;
    int y;
};

int sum(int a,
        int b) {
    return a + b;
}

void test() {
    int values[] = {
        1,
        2,
    };
    [[maybe_unused]] int total = sum(values[0],
                                     values[1]);
}

}  // namespace demo
