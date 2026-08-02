/// # Filtering & Ranking
///
/// ## Case-insensitive prefix — a lowercase prefix matches a mixed-case identifier
///
/// - status: supported
/// - order: 11

// error-ok: the completion prefix dangles as an unfinished statement.
int MyLongName;

void bar() {
    int v = mylong§(pos);
}
