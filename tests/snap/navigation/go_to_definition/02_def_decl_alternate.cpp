/// # Definition and declaration alternate
///
/// - status: supported
/// - verify: server
///
/// Navigation alternates between a declaration and definition
///
/// A request from a use reaches the definition, while requests at the
/// declaration or definition reach the other site. An inline symbol with
/// no separate declaration keeps its definition as the answer.

int §(decl)scale(int value);

int §(def)scale(int value) {
    return value * 2;
}

int apply(int value) {
    return §(use)scale(value);
}
