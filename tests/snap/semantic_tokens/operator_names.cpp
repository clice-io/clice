/// # Token Correctness
///
/// ## Operator names — `operator` in a declaration keeps the method kind
///
/// - status: partial
/// - order: 3
///
/// The `operator` token of a declaration currently renders as `conflict`
/// (its keyword nature collides with the declared method). Operator call
/// sites correctly emit no token on the punctuation.

struct Value {
    Value& §operator=(const Value& other);
    Value §operator+(const Value& other) const;
};

void combine(Value a, Value b) {
    a §= b;
    Value c = a §+ b;
}
