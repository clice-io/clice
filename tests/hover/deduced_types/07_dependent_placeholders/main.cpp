// Placeholders a template leaves undeduced: a variable whose auto and a
// variable whose class template arguments depend on the template's
// parameter. Their cards once crashed the server. The `auto` keyword shows
// the type as the template writes it, without the declarator's references,
// pointers and qualifiers.

namespace dependent_placeholders {

template <typename T> struct Wrapper {
    Wrapper(T);
};

template <typename T> void make(T value, T* source) {
    §(dependent_keyword)auto §(dependent_auto)copy = value;
    const §(reference_keyword)auto& reference = value;
    §(pointer_keyword)auto* pointer = source;
    §(forwarding_keyword)auto&& forwarded = value;
    Wrapper §(dependent_class_arguments)wrapper{value};
}

}
