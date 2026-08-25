/// # Document Highlight
///
/// ## Control flow token highlighting
///
/// - status: unsupported
/// - order: 3
/// - issues: clangd#1921
///
/// Highlighting `break` or `continue` should also light up the loop or
/// `switch` it belongs to — and `return` / `throw` the function exits
/// they mark.

void drain(int outer, int inner) {
    for (int i = 0; i < outer; i += 1) {
        for (int j = 0; j < inner; j += 1) {
            if (i == j) {
                break;      // highlighting break → also the inner for
            }
            if (j == 0) {
                continue;   // highlighting continue → also the inner for
            }
        }
    }
}
