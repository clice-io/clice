/// # Filtering & Ranking
///
/// ## Case-insensitive prefix — a lowercase prefix matches a mixed-case identifier
///
/// - status: supported
/// - order: 11
/// - diagnostics: expected

// The completion prefix dangles as an unfinished statement.
int MyLongName;

void bar() {
    int v = mylong§(pos);
}
