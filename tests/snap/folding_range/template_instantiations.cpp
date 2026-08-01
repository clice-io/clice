/// # Fold Kinds
///
/// ## Template specializations and instantiations — written specializations and their members fold; instantiated declarations reuse the pattern's source locations and must not fold it again
///
/// - status: supported
/// - order: 13

template <typename T>
struct Box {
    T value;

    void reset() {
        value = T();
    }
};

template <>
struct Box<void> {
    void reset() {
        // nothing stored
    }
};

template <typename T>
struct Box<T*> {
    T* pointee;
};

// Neither the implicit instantiation Box<int> nor the explicit instantiation
// Box<char> re-folds the primary's braces or the reset() body.
Box<int> implicit_use;
template struct Box<char>;
