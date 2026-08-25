/// # Go to Definition
///
/// ## Definition and declaration alternate at the cursor site
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// On a use, go-to-definition reaches the definition. Invoked on the
/// definition it steps to the declaration, and on the declaration it
/// steps to the definition — the two sites alternate. A symbol defined
/// inline, with no separate declaration, keeps its definition as the
/// answer.

int §(decl)scale(int value);

int §(def)scale(int value) {
    return value * 2;
}

int apply(int value) {
    return §(use)scale(value);
}
