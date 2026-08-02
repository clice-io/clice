/// # Symbols
///
/// ## Local shadowing a global — the shadowed global does not appear as a duplicate entry
///
/// - status: supported
/// - order: 12

// error-ok: the completion prefix dangles as an unfinished statement.
int counter = 0;

void bar() {
    int counter = 1;
    int v = coun§(pos);
}
