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
