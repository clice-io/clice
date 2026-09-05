/// # Pass semantics
///
/// - status: supported
///
/// By value, by reference, by const reference
///
/// The argument card states how the value reaches the callee: copied by
/// value, or bound to a mutable or const reference parameter.

namespace argument_transfer {
void consume(long value);
void mutate(long& value);
void observe(const long& value);

void apply() {
    long count = 1;
    consume(§(01_value)count);
    mutate(§(02_ref)count);
    observe(§(03_const_ref)count);
}
}
