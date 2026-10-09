/// # Object-like vs function-like macros
///
/// - status: unsupported
/// - issues: clangd#2649
///
/// Object-like and function-like macros do not receive distinct token kinds yet

#define MAX_SIZE 1024
#define CHECK(x) ((x) ? 1 : 0)

int checked = CHECK(MAX_SIZE);
