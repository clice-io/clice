/// # Non-ASCII prefix
///
/// - status: supported
/// - diagnostics: expected
///
/// A prefix made of non-ASCII identifier characters is replaced, not
/// inserted before

// The completion prefix dangles as an unfinished statement.
int 变量一 = 0;

void bar() {
    int v = 变§(pos);
}
