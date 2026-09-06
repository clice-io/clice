/// # Explicit instantiation
///
/// - status: supported
/// - issues: clangd#316
///
/// Explicit class instantiations highlight template names and written arguments

struct Widget {};

template <typename T>
struct Holder {
    T value;
};

extern template struct §Holder<§Widget>;

template struct §Holder<§Widget>;
