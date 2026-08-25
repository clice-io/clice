/// # Find References
///
/// ## Enclosing function shown with each reference
///
/// - status: unsupported
/// - order: 8
/// - issues: clangd#177
///
/// Each reference is reported as a bare location; the name of the function
/// that encloses it is not attached, so results carry no context beyond
/// the file and line.

int shared_value = 0;

int reader() {
    return shared_value;
}

int writer() {
    shared_value = 1;
    return shared_value;
}
