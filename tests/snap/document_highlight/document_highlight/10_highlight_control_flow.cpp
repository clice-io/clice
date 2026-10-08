/// # Control flow token highlighting
///
/// - status: unsupported
/// - issues: clangd#1921
///
/// Control-flow keywords have no related document highlights yet

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
