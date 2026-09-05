/// # Anonymous parameters
///
/// - status: supported
///
/// Nothing to name, though a mutable reference still flags `&`

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
