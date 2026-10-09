/// # Dependent return type
///
/// - status: supported
///
/// A return type naming the class template or one of its member types is qualified through the template's parameters

template <typename T>
struct Container {
    using value_type = T;
    value_type §(front)front() const;
};
