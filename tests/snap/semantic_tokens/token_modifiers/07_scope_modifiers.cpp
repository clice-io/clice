/// # Scope modifiers
///
/// - status: unsupported
/// - issues: clangd#352
///
/// function, class, file and global scope

int global_scope;
static int file_scope;

struct Foo {
    int class_scope;

    void bar() {
        int function_scope = 0;
    }
};
