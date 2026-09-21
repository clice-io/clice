/// # Trailing comments stay in place
///
/// - status: supported
///
/// A comment ending a definition's line moves with that definition, never with the one below it

struct §(cls)S {
    void first();
    void second();
};

void S::second() {
}  // closes second

void S::first() {
}
