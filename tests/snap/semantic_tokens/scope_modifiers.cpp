/// # Token Modifiers
///
/// ## Scope modifiers — function, class, file and global scope
///
/// - status: unsupported
/// - issues: clangd#352
/// - order: 7

int global_scope;
static int file_scope;

struct Foo {
    int class_scope;

    void bar() {
        int function_scope = 0;
    }
};
