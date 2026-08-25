/// # Document Highlight
///
/// ## Highlight every reference to the symbol under the cursor in the current file
///
/// - status: unsupported
/// - order: 1
///
/// Placing the cursor on `total` should light up its declaration and
/// every use in the file; the request is not implemented.

int total = 0;

void accumulate(int amount) {
    total = total + amount;
}
