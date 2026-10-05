/// # Missing members from a definition
///
/// - status: supported
///
/// Inside an out-of-line definition, the class's remaining undefined members are offered too
///
/// This is how a source file completes a class declared elsewhere: the
/// definitions join the ones already there.

struct Parser {
    void parse();
    void reset();
    bool done() const;
};

void Parser::§(parse)parse() {
}
