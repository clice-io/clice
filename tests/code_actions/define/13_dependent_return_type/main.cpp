// A return type naming a member type of the class template is qualified through
// the template's parameters, with `typename`.

template <typename T>
struct Container {
    using value_type = T;
    using size_type = unsigned long;
    struct Node {};
    template <typename U>
    using Alias = U;
    value_type §(front)front() const;
    size_type §(size)size() const;
    const Node* §(head)head();
    Container §(clone)clone();
    Alias<T> §(alias)alias();
};
