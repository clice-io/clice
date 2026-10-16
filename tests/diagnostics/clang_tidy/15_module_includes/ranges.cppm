export module ranges;

export namespace std {
template <class T> struct vector {
    T* rbegin();
    T* rend();
};
template <class I> void sort(I first, I last);
}
