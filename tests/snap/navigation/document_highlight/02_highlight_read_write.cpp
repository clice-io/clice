/// # Read/write classification for symbol highlights
///
/// - status: unsupported
///
/// Each highlight should carry its access kind, so editors can tint
/// writes differently from reads

void tally() {
    int count = 0;      // write
    int next = count;   // read
    count = next;       // write
}
