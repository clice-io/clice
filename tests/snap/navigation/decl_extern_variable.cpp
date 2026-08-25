/// # Go to Declaration
///
/// ## `extern` variable — to the declaration
///
/// - status: supported
/// - verify: server
/// - order: 5
///
/// A use of an `extern` variable offers the `extern` declaration and
/// the defining declaration together, so the header-side declaration is
/// always reachable from a use.

extern int §(decl)log_level;

int §(def)log_level = 0;

int read_level() {
    return §(use)log_level;
}
