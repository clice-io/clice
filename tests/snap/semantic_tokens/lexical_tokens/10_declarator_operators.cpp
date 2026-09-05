/// # Declarator vs operator disambiguation
///
/// - status: unsupported
/// - issues: clangd#1421
///
/// Declarator and expression operators do not receive distinct token kinds yet

int value = 1;
int* pointer = &value;
int& reference = value;
int product = value * value;
int masked = value & 1;
