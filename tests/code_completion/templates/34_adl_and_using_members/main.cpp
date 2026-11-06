namespace lib {

struct Item {
    int item_value;
};

template <class T>
struct Bag {};

template <class T>
T* begin(Bag<T>& bag);

template <class T>
T* end(Bag<T>& bag);

template <class T>
struct Cursor {};

template <class T>
T& operator*(Cursor<T> cursor);

}  // namespace lib

struct Other {
    int other_value;
};

template <class T>
int each(lib::Bag<T>& bag) {
    for(auto& item: bag) {
        return item.§(adl_loop);
    }
    return 0;
}

template <class T>
int first(lib::Bag<T>& bag) {
    return begin(bag)->§(adl_begin);
}

template <class T>
int read(lib::Cursor<T> cursor) {
    return (*cursor).§(free_dereference);
}

template <class T>
struct Base {
    lib::Item get(int index);
};

template <class T>
struct Derived : Base<T> {
    using Base<T>::get;
    Other get(double ratio);

    int use() {
        return this->get(1).§(using_beside_own);
    }
};

int use() {
    lib::Bag<lib::Item> bag;
    each(bag);
    first(bag);
    read(lib::Cursor<lib::Item>{});
    return Derived<lib::Item>{}.use();
}
