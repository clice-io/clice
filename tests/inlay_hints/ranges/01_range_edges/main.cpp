#define CHECK(expr)                                                                                \
    if(!(expr)) {                                                                                  \
    }

int scale(int value, int factor);

struct Settings {
    int width;
    int height;
    int depth;
§(struct_end)⟦};
⟧
int compute(int base) {
    auto doubled = scale(base, 2);
    CHECK(scale(doubled,
§(macro_tail)⟦                3) > 0);
⟧    auto tripled = scale(doubled, 3);
    §(function_tail)⟦return tripled;
}
⟧
int unrelated(int base) {
    return scale(base, 4);
}

constexpr int align(int value, int factor) {
    return value * factor;
}

§(attribute)⟦alignas(align(2, 4))⟧
int storage;

int padded
§(trailing_attribute)⟦    __attribute__((aligned(align(4, 2))));⟧

void sink(int value);

void boundaries() {
    §(ends_at_argument)⟦sink(⟧1);
    sink(§(starts_at_argument)⟦1⟧);
}
