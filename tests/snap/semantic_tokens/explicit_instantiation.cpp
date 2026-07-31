/// # Declarations & References
///
/// ## Explicit instantiation — the instantiated template name highlighted
///
/// - status: supported
/// - issues: clangd#316
/// - order: 15

template <typename T>
struct Holder {
    T value;
};

template struct §Holder<int>;
