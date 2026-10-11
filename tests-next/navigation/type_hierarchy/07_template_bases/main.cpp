// A class deriving from a specialization of a class template, or from the
// template through its own parameter, is a subtype of the template.

template <typename T>
struct §(base)Base {};

struct §(derived)Derived : Base<int> {};

template <typename T>
struct §(generic)Generic : Base<T> {};

Generic<char> g;
