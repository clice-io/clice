/// # Call parentheses
///
/// - status: supported
/// - config: {"insert_paren_in_function_call": true}
/// - diagnostics: expected
///
/// A completed call gets its parentheses with the cursor between them, unless
/// the name is already followed by one

// The completion prefixes dangle as unfinished statements.
int compute(int x);

void bar() {
    int a = compu§(bare);
    int b = compu§(followed)(1);
}
