/// # Find References
///
/// ## References in dependent and template contexts
///
/// - status: unsupported
/// - order: 6
/// - issues: clangd#258, clangd#675
///
/// Find references on a member does not include dependent call sites in a
/// template, even when the template is instantiated with the member's
/// class.

struct A {
    void foo();  // find-refs here omits the dependent obj.foo() below
};

template <typename T>
void process(T& obj) {
    obj.foo();
}

void run(A a) {
    process(a);
}
