/// # Declarations & References
///
/// ## Labels — `goto` targets and label definitions
///
/// - status: supported
/// - order: 7

void retry(bool again) {
    goto §done;
§done:
    if (again) {
        goto done;
    }
}
