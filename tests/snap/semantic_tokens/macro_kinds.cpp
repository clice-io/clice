/// # Macros
///
/// ## Object-like vs function-like macros — distinct highlighting for the two forms
///
/// - status: unsupported
/// - issues: clangd#2649
/// - order: 3

#define MAX_SIZE 1024
#define CHECK(x) ((x) ? 1 : 0)

int checked = CHECK(MAX_SIZE);
