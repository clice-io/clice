// A requires-expression's braces span lines like any other block.
template <typename T>
concept Shape = requires(T t) {
    t.area();
    t.perimeter();
};

template <typename T>
    requires requires(T t) {
        t.area();
    }
void draw(T shape);

template <typename T>
void probe(T value) {
    if constexpr (requires {
                      value.area();
                  }) {
        value.area();
    }
}
