/// # Declarations & References
///
/// ## Explicit instantiation — the instantiated template name and its written template arguments highlighted, on the extern declaration and the definition alike
///
/// - status: supported
/// - issues: clangd#316
/// - order: 15

struct Widget {};

template <typename T>
struct Holder {
    T value;
};

extern template struct §Holder<§Widget>;

template struct §Holder<§Widget>;
