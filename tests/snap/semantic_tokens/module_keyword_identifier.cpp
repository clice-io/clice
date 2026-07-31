/// # Modules
///
/// ## `module` and `import` as identifiers — contextual keywords keep their semantic kinds outside module declarations
///
/// - status: supported
/// - order: 3

void f() {
    struct §module {};
    §module §m;
    int §import = 1;
    int §module = 2;
}
