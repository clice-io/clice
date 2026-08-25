/// # Implicit Code Navigation
///
/// ## User-defined literals — navigate to the literal operator
///
/// - status: unsupported
/// - order: 16
///
/// Go-to-definition on a user-defined-literal suffix should reach its
/// `operator""`; today it returns nothing.

struct Duration {
    unsigned long long ticks;
};

Duration operator""_ms(unsigned long long value);

void use() {
    Duration d = 500_ms;  // go-to-def on _ms → operator""_ms
}
