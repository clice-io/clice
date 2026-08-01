/// # Fold Kinds
///
/// ## Abbreviated function templates — bodies of functions with `auto` or constrained `auto` parameters fold like any other function
///
/// - status: supported
/// - order: 13

template <typename T>
concept Small = sizeof(T) <= 8;

void consume(Small auto x) {
    auto copy = x;
    copy += 1;
}

void forward(auto value) {
    consume(value);
}
