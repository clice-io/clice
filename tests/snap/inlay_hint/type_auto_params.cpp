/// # Type Hints
///
/// ## `auto` parameters — a template with exactly one instantiation reveals the deduced type
///
/// - status: supported
/// - order: 7

int twice(auto x) {
    return x + x;
}

int result = twice(21);

// A second instantiation makes the deduction ambiguous: no hint.
int measure(auto x) {
    return 1;
}

int a = measure(1);
int b = measure(2.0);

// Packs and parameters after them never hint.
int spread(auto first, auto... rest, auto last) {
    return 0;
}

int c = spread<void*, char, float>(nullptr, 'x', 2.0f, 3);

// Deduplication: a template body hints once across instantiations of the
// same deduced type.
template <typename T>
void body() {
    auto var = 42;
}

template void body<int>();
template void body<float>();
