/// # Read/write classification for symbol highlights
///
/// - status: unsupported
///
/// Document highlights do not report read and write access kinds yet

void tally() {
    int count = 0;      // write
    int next = count;   // read
    count = next;       // write
}
