/// # `break` / `continue`
///
/// - status: unsupported
/// - issues: clangd#1921
///
/// `break` and `continue` do not navigate to their enclosing control statement
/// yet
///
/// Go-to-definition on `break` or `continue` does not reach the head of the
/// loop or switch it controls.

void loop() {
    for (int i = 0; i < 10; i += 1) {
        if (i == 5) break;  // go-to-def on break → the for loop
        continue;           // go-to-def on continue → the for loop
    }
}
