/// # Declarator vs operator disambiguation
///
/// - status: unsupported
/// - issues: clangd#1421
///
/// `*`, `&`, `&&` as declarators vs arithmetic/logical operators

int value = 1;
int* pointer = &value;
int& reference = value;
int product = value * value;
int masked = value & 1;
