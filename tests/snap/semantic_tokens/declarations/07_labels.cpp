/// # Labels
///
/// - status: supported
///
/// Labels and their `goto` references receive label tokens

void retry(bool again) {
    goto §done;
§done:
    if (again) {
        goto done;
    }
}
