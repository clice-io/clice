// Explicit instantiation directives: the card of the class, function and
// variable each instantiates, and of a template argument in one.

namespace explicit_instantiation {
struct Item {};
template <typename T> struct Box {};
extern template struct §(extern_class_directive)Box<§(class_directive_argument)Item>;
template struct §(class_directive)Box<Item>;
template <typename T> void run(T) {}
template void §(function_directive)run<long>(long);
template <typename T> long zero = 0;
template long §(variable_directive)zero<long>;
}
