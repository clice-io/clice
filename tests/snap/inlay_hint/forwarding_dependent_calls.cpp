// Packs forwarded through calls left dependent inside a generic lambda: an
// overloaded name resolves to its only candidate taking as many arguments,
// and a member call whose explicit object is not among its arguments
// resolves to nothing.

void sink(int width, int height, int tag);
void sink(int width);

struct Panel {
    void place(this Panel& self, int left, int top, int tag);
};

Panel panel;

template <typename... Args>
auto overloaded(Args... args) {
    return [=](auto tag) { ::sink(args..., tag); };
}

template <typename... Args>
auto member(Args... args) {
    return [=](auto tag) { panel.place(args..., tag); };
}

void use() {
    overloaded(1, 2);
    member(3, 4);
}
