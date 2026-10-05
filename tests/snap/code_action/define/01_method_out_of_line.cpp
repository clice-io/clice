/// # Define a declared method
///
/// - status: supported
/// - issues: clangd#445
///
/// A method declaration offers an inline body and an out-of-line definition after its class
///
/// The out-of-line definition repeats the declaration with `S::` before the
/// name and drops what belongs to the declaration alone, such as default
/// arguments.

struct S {
    int §(f)f(int x, int y = 3) const;
};
