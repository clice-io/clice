// A specialization the code only names, through a pointer, is no
// instantiation: Holder<char> stays the only one.
template <typename T>
struct Holder {
    void reset() {
        §(only)⟦auto copy = T();⟧
    }
};

template struct Holder<char>;

Holder<int>* elsewhere;
