/// # Functions & Snippets
///
/// ## Default-argument parameters — a parameter with a default value drops out of the signature detail
///
/// - status: supported
/// - order: 10
/// - diagnostics: expected
///
/// The signature detail keeps only the required parameters; the trailing
/// `int retries = 3` is elided.

// The completion prefix cuts the initializer mid-expression.
int configure(int timeout, int retries = 3);

int x = confi§(pos)
