/// # Object-like vs function-like macros
///
/// - status: unsupported
/// - issues: clangd#2649
///
/// Distinct highlighting for the two forms

#define MAX_SIZE 1024
#define CHECK(x) ((x) ? 1 : 0)

int checked = CHECK(MAX_SIZE);
