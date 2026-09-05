/// # Explicit instantiation
///
/// - status: supported
/// - issues: clangd#316
///
/// The instantiated template name and its written template arguments highlighted, on the extern declaration and the definition alike

struct Widget {};

template <typename T>
struct Holder {
    T value;
};

extern template struct §Holder<§Widget>;

template struct §Holder<§Widget>;
