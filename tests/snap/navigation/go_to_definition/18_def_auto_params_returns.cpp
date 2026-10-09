/// # `auto` parameters and return types
///
/// - status: supported
/// - verify: server
///
/// The `auto` of an abbreviated template's parameter reaches the type of its
/// only instantiation, and an `auto` return type the type returned
///
/// A parameter whose template is instantiated with several types reaches
/// none of them. The leading `auto` of a trailing return type reaches the
/// type after the arrow.

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
