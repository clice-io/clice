/// # Case-insensitive prefix
///
/// - status: supported
/// - diagnostics: expected
///
/// A lowercase prefix matches a mixed-case identifier

// The completion prefix dangles as an unfinished statement.
int MyLongName;

void bar() {
    int v = mylong§(pos);
}
