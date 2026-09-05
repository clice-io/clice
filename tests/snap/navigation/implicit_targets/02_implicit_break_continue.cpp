/// # `break` / `continue`
///
/// - status: unsupported
/// - issues: clangd#1921
///
/// Navigate to the enclosing loop or switch head
///
/// Go-to-definition on `break` or `continue` should reach the head of the
/// loop or switch it controls; today it returns nothing.

void loop() {
    for (int i = 0; i < 10; i += 1) {
        if (i == 5) break;  // go-to-def on break → the for loop
        continue;           // go-to-def on continue → the for loop
    }
}
