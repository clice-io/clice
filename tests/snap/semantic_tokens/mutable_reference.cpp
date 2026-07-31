/// # Token Modifiers
///
/// ## Mutable reference and pointer — arguments passed by non-const reference or pointer
///
/// - status: unsupported
/// - issues: clangd#839
/// - order: 8

void modify(int& out);
void modify_through(int* out);
void inspect(const int& in);

void run() {
    int value = 0;
    modify(value);
    modify_through(&value);
    inspect(value);
}
