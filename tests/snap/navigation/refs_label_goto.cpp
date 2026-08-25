/// # Find References
///
/// ## Label and goto references
///
/// - status: supported
/// - verify: server
/// - order: 11
///
/// Find references on a label lists the label itself together with every
/// `goto` that jumps to it.

int loop(int failed) {
    §(label)retry:
    if (failed) {
        goto retry;
    }
    return 0;
}
