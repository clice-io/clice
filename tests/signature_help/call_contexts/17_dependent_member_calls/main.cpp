template <class T>
struct List {
    T& operator[](int index);
    void push(const T& value, int count);
    void push(const T& value);
};

template <class T>
void fill(List<T> items, List<List<T>>& rows, T value) {
    items.push(§(member)value, 1);
    items.push(value, §(second)1);
    rows[0].push(§(nested)value, 1);
}
