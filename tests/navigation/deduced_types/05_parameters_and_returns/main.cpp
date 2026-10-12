struct Widget {};

struct Gadget {};

void once(au§(single_instantiation)to value) {}

void twice(au§(several_instantiations)to value) {}

au§(deduced_return)to make() {
    return Widget{};
}

au§(trailing_return)to build() -> Gadget;

void use() {
    once(Widget{});
    twice(Widget{});
    twice(Gadget{});
    auto lambda = [](au§(generic_lambda)to item) {};
    lambda(Gadget{});
}
