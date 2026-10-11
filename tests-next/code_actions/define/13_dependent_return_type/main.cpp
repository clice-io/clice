// A return type naming a member type of the class template is qualified through
// the template's parameters, with `typename`.

template <typename T>
struct Container {
    using value_type = T;
    value_type §(front)front() const;
};
