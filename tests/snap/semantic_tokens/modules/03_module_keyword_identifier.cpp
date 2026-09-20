/// # `module` and `import` as identifiers
///
/// - status: supported
///
/// Contextual keywords keep their semantic kinds outside module declarations

void f() {
    struct §module {};
    struct §module §m;
    int §import = 1;
    int §module = 2;
}
