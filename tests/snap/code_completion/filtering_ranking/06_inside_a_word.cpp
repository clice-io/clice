/// # Completion inside a word
///
/// - status: supported
///
/// Completing in the middle of a word offers both ranges: the editor either
/// inserts before the rest of the word or replaces the whole word

// The completion point splits an existing identifier.
int balance = 0;

void bar() {
    int v = bal§(pos)ance;
}
