/// # Statement keywords
///
/// - status: supported
/// - config: {"enable_keyword_snippet": true}
/// - diagnostics: expected
///
/// Statement keywords complete as keywords; the option inserts the whole
/// statement with a placeholder for each part

// The completion prefix dangles as an unfinished statement.
void bar() {
    whi§(pos);
    fo§(variants);
}
