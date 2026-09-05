/// # Mutable reference and pointer
///
/// - status: unsupported
/// - issues: clangd#839
///
/// Mutable reference and pointer arguments do not carry a modifier yet

void modify(int& out);
void modify_through(int* out);
void inspect(const int& in);

void run() {
    int value = 0;
    modify(value);
    modify_through(&value);
    inspect(value);
}
