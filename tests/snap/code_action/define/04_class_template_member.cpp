/// # Class template member
///
/// - status: supported
///
/// The definition of a class template's member carries the template head and the template arguments in its qualifier
///
/// Default template arguments are not repeated on the head.

template <typename T, int N = 4>
struct Box {
    T §(get)get() const;
};

template <typename T>
struct Box<T*, 1> {
    T* §(single)single();
};
