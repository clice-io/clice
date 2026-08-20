/// # Type Hints
///
/// ## Instantiated templates — instantiated bodies repeat no hints at the pattern; dependent `auto` could reveal the deduced type while exactly one instantiation exists
///
/// - status: partial
/// - issues: clangd#2275
/// - order: 12

void take(int first, int second);

template <typename T>
struct Single {
    void reset() {
        take(1, 2);
        // Deducible from the only instantiation, but not yet deduced.
        auto copy = T();
    }
};

template struct Single<char>;

template <typename T>
struct Twice {
    void reset() {
        // No hint: two instantiations deduce contradicting types.
        auto copy = T();
    }
};

template struct Twice<char>;
template struct Twice<int>;
