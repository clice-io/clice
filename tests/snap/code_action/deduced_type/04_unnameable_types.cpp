/// # Unnameable types stay auto
///
/// - status: supported
///
/// Lambdas, dependent types and other types without a spelling are not expanded

template <typename T>
void g(T value) {
    §(dependent)auto copy = value;
}

void f() {
    §(lambda)auto callback = [] {};
}
