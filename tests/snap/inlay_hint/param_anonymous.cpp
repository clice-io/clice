/// # Parameter Hints
///
/// ## Anonymous parameters — nothing to name, though a mutable reference still flags `&`
///
/// - status: supported
/// - order: 17

void value_sink(int);
void ref_sink(int&);
void const_ref_sink(const int&);
void rvalue_sink(int&&);

void use() {
    int v = 0;
    value_sink(1);
    // Only the `&` marker survives without a name.
    ref_sink(v);
    const_ref_sink(v);
    rvalue_sink(2);
}
