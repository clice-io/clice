/// # Local shadowing a global
///
/// - status: supported
/// - diagnostics: expected
///
/// The shadowed global does not appear as a duplicate entry

// The completion prefix dangles as an unfinished statement.
int counter = 0;

void bar() {
    int counter = 1;
    int v = coun§(pos);
}
