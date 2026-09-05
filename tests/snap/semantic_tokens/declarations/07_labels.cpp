/// # Labels
///
/// - status: supported
///
/// `goto` targets and label definitions

void retry(bool again) {
    goto §done;
§done:
    if (again) {
        goto done;
    }
}
