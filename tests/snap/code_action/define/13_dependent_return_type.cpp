/// # Dependent return type
///
/// - status: partial
///
/// A dependent return type stays as written, which may need `typename` and qualification outside the class

template <typename T>
struct Container {
    using value_type = T;
    value_type §(front)front() const;
};
