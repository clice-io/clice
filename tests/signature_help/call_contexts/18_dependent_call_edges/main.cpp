struct Base {
    void add(int amount);
};

template <class T>
struct Bag : Base {
    using Base::add;
    void add(const T& item, int count);
};

template <class T>
struct Node {
    Node(T value, int depth);
};

namespace store {

template <class T, class... Ts>
void fill(Bag<T> bag, Ts... items, T item) {
    bag.add(§(brought_in)1);
    bag.add(items..., §(after_pack)1);
    bag.add(it§(typing)em, 1);
    bag.add(item, 1§(typing_number));
    bag.add(Node<T>(item, §(inner_unanswered)1), 1);
}

}  // namespace store
