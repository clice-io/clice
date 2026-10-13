// Placeholders a template leaves undeduced: a variable whose auto and a
// variable whose class template arguments depend on the template's
// parameter. Their cards once crashed the server. The `auto` keyword shows
// the type as the template writes it.

namespace dependent_placeholders {

template <typename T> struct Wrapper {
    Wrapper(T);
};

template <typename T> void make(T value) {
    §(dependent_keyword)auto §(dependent_auto)copy = value;
    Wrapper §(dependent_class_arguments)wrapper{value};
}

}
