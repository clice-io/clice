/// # Label and goto references
///
/// - status: supported
/// - verify: server
///
/// Find references on a label lists the label itself together with every `goto`
/// that jumps to it

int loop(int failed) {
    §(label)retry:
    if (failed) {
        goto retry;
    }
    return 0;
}
