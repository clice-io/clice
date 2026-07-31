/// # Lexical Tokens
///
/// ## Declarator vs operator disambiguation — `*`, `&`, `&&` as declarators vs arithmetic/logical operators
///
/// - status: unsupported
/// - issues: clangd#1421
/// - order: 8

int value = 1;
int* pointer = &value;
int& reference = value;
int product = value * value;
int masked = value & 1;
