/// # Labels — `goto` targets and label definitions
///
/// - status: supported

void retry(bool again) {
    goto §done;
§done:
    if (again) {
        goto done;
    }
}
