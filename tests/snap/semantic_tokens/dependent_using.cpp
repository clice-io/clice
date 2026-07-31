/// # Declarations & References
///
/// ## Dependent using declarations — `using T::name` in a template body
///
/// - status: partial
/// - order: 25
///
/// The introduced name and its uses currently get no token; the reserved
/// dependent-name modifier is not emitted yet.

template <typename T>
struct Derived : T {
    using T::§value;

    int use() {
        return §value;
    }
};
