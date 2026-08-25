/// # Call Hierarchy
///
/// ## Non-function targets — variables and enum constants
///
/// - status: unsupported
/// - order: 7
/// - issues: clangd#1308
///
/// Preparing a call hierarchy on a variable or an enum constant returns
/// nothing; the request is offered only for functions and methods.

int counter = 0;  // prepare call hierarchy here → nothing

enum Mode {
    Fast,  // prepare call hierarchy here → nothing
    Slow,
};
