/// # User-defined literals
///
/// - status: unsupported
///
/// A literal suffix does not navigate to its user-defined literal operator yet
///
/// Go-to-definition on a user-defined-literal suffix does not reach its
/// `operator""`.

struct Duration {
    unsigned long long ticks;
};

Duration operator""_ms(unsigned long long value);

void use() {
    Duration d = 500_ms;  // go-to-def on _ms → operator""_ms
}
