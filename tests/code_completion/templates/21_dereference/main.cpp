// A dereferenced dependent pointer or iterator, then a member access.
template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
struct Cursor {
    T& operator*() const;
};

template <class T>
void walk(Wrap<T>* pointer, Cursor<Wrap<T>> cursor) {
    (*pointer).§(pointer);
    (*cursor).§(iterator);
}
