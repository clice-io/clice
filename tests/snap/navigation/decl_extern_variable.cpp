/// # Go to Declaration
///
/// ## `extern` variable — to the declaration
///
/// - status: supported
/// - verify: server
/// - order: 5
///
/// An `extern` variable's declaration and its defining declaration
/// alternate from a use, so go-to-declaration on a use of the variable
/// reaches the `extern` declaration.

extern int §(decl)log_level;

int §(def)log_level = 0;

int read_level() {
    return §(use)log_level;
}
