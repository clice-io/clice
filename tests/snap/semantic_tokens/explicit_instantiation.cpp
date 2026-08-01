/// # Declarations & References
///
/// ## Explicit instantiation — the instantiated template name and its written template arguments highlighted
///
/// - status: supported
/// - issues: clangd#316
/// - order: 15

struct Widget {};

template <typename T>
struct Holder {
    T value;
};

template struct §Holder<§Widget>;
