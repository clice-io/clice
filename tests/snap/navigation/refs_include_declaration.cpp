/// # Find References
///
/// ## Declaration and definition sites appear among references
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// A reference query returns the declaration and the out-of-line
/// definition together with every use, so the whole surface of a symbol
/// is reachable from any one of its sites.

int §(decl)scale(int value);

int §(def)scale(int value) {
    return value * 2;
}

int use() {
    return §(use)scale(3);
}
