/// # Find References
///
/// ## Read/write classification of references
///
/// - status: unsupported
/// - order: 7
/// - issues: clangd#2139
///
/// The reference reply carries only locations, so a reader cannot tell a
/// write from a read; annotating each result with its access kind is not
/// offered.

int use() {
    int x = 0;      // write
    int y = x + 1;  // read
    x = y;          // write
    return x;
}
