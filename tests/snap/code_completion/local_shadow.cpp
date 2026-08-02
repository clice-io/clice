/// # Symbols
///
/// ## Local shadowing a global — both the local and the global name it shadows complete
///
/// - status: supported
/// - order: 12

// error-ok: the completion prefix dangles as an unfinished statement.
int counter_global;

void bar() {
    int counter_local;
    int v = coun§(pos);
}
