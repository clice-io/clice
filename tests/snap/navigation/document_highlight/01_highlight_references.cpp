/// # Highlight every reference to the symbol under the cursor in the current file
///
/// - status: unsupported
///
/// Placing the cursor on `total` should light up its declaration and
/// every use in the file; the request is not implemented.

int total = 0;

void accumulate(int amount) {
    total = total + amount;
}
