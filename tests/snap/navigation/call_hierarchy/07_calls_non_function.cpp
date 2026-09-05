/// # Non-function targets
///
/// - status: unsupported
/// - issues: clangd#1308
///
/// Variables and enum constants
///
/// Preparing a call hierarchy on a variable or an enum constant returns
/// nothing; the request is offered only for functions and methods.

int counter = 0;  // prepare call hierarchy here → nothing

enum Mode {
    Fast,  // prepare call hierarchy here → nothing
    Slow,
};
