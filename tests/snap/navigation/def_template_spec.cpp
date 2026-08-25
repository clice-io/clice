/// # Go to Definition
///
/// ## Template specialization navigates to the primary template
///
/// - status: unsupported
/// - order: 11
/// - issues: clangd#212
///
/// Go-to-definition on the name of an explicit specialization resolves to
/// the specialization itself; stepping from it to the primary template it
/// specializes is not offered.

template <typename T>
struct Formatter {}; // primary template

template <>
struct Formatter<int> {}; // go-to-def on Formatter → primary template
