/// # Scope modifiers
///
/// - status: unsupported
/// - issues: clangd#352
///
/// Symbols do not carry function, class, file or global scope modifiers yet

int global_scope;
static int file_scope;

struct Foo {
    int class_scope;

    void bar() {
        int function_scope = 0;
    }
};
