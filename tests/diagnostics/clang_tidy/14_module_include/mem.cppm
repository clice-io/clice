export module mem;

export namespace std {
template <class T> struct default_delete {};
template <class T, class D = default_delete<T>> struct unique_ptr {
    explicit unique_ptr(T*);
    ~unique_ptr();
};
}
