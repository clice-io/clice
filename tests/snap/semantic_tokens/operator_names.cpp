/// # Token Correctness
///
/// ## Operator names — the `operator` keyword and call-site punctuation stay plain
///
/// - status: supported
/// - order: 3
///
/// An operator's written name is keyword plus punctuation, so no name
/// token is painted: `operator` keeps its keyword classification and
/// call sites emit nothing on the operator symbol.

struct Value {
    Value& §operator=(const Value& other);
    Value §operator+(const Value& other) const;
};

void combine(Value a, Value b) {
    a §= b;
    Value c = a §+ b;
}
