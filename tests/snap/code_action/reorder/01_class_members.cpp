/// # Reorder definitions by declaration
///
/// - status: supported
///
/// The out-of-line definitions of a class's members are reordered to follow the declaration order in the class
///
/// Each definition moves with the comment block directly above it.

struct §(cls)Parser {
    void first();
    void second();
    void third();
};

/// Parses the tail.
void Parser::third() {
}

void Parser::first() {
}

// The middle one.
void Parser::§(second)second() {
}
