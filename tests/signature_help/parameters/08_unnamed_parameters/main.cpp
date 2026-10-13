struct Range {
    void resize(int count);
};

void Range::resize(int) {}

template <typename T>
struct Box {
    void fill(int, T);
};

template <typename T>
void Box<T>::fill(int count, T value) {}

template <typename T>
const T& larger(const T&, const T&);

template <typename T>
const T& larger(const T& a, const T& b) {
    return a < b ? b : a;
}

void use(Range range, Box<int> box) {
    range.resize(§(definition_unnamed)1);
    box.fill(1, §(declaration_unnamed)2);
    larger(§(template_redeclared)1, 2);
}
