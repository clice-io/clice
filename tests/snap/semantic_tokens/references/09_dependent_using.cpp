/// # Dependent using declarations
///
/// - status: partial
///
/// Dependent using declarations remain unpainted
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
