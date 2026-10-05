/// # Concrete alias target
///
/// - status: supported
/// - diagnostics: expected
///
/// A dependent alias that names a concrete specialization completes the
/// members that specialization instantiates to
///
/// The specialization is instantiated for the completion when the file never
/// did, so its members carry the concrete argument types, and a partial
/// specialization matching the arguments supplies them.

// Both member accesses dangle; the statements stay semicolon-terminated so
// the second marker is not dragged into recovery.
template <typename T>
struct Box {
    T value;

protected:
    int guarded;
};

template <typename T>
struct Box<T*> {
    T* pointee;
};

template <typename T>
struct Holder {
    using plain = Box<int>;
    using pointer = Box<char*>;
};

template <typename T>
void bar() {
    typename Holder<T>::plain p;
    p.§(plain);
    typename Holder<T>::pointer q;
    q.§(pointer);
}
