// Types a compiler builtin transforms, as libc++'s add_pointer_t does.
template <class T>
using add_pointer_t = __add_pointer(T);

template <class T>
using remove_cvref_t = __remove_cvref(T);

template <class T>
struct Holder {
    add_pointer_t<T> operator->();
    remove_cvref_t<const T&> copy();
};

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
void walk(Holder<Wrap<T>> holder) {
    holder->§(add_pointer);
    holder.copy().§(remove_cvref);
}
