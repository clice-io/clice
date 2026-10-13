// An iterator a class template defines inside itself, reached through a
// dependent specialization: what its `->` and `*` return is written in the
// enclosing template's parameter.
template <class T>
struct List {
    struct iterator {
        T* operator->() const;
        T& operator*() const;
    };
    iterator begin();
};

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
void walk(List<Wrap<T>> list) {
    list.begin()->§(call_arrow);
    (*list.begin()).§(call_star);
    typename List<Wrap<T>>::iterator it;
    it->§(named_type);
}
