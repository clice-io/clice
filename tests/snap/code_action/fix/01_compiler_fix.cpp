/// # Compiler fix
///
/// - status: supported
/// - diagnostics: expected
///
/// A fix the compiler attaches to its diagnostic is offered as a quick fix
///
/// The title spells out a single edit.

int answer() {
    return 42§(semicolon)
}
