template <typename T>
struct Box {
    Box(T first, T second);
};

template <typename T>
struct Policy {};

template <typename T, typename P = Policy<T>>
struct Owner {};

template <typename T>
Owner<T> make_owner();

template <typename T, typename U>
struct Table {};

template <typename T>
Table<T, int> make_table();

template <typename T>
T* create();

int compute();

void use() {
    §(spelled)⟦auto widened = static_cast<long>(compute());
    auto braced = int{42};
    auto boxed = Box<int>(1, 2);
    auto listed = Box<int>{3, 4};
    auto grouped = (long{5});
    auto created = new Box<int>(5, 6);
    auto owned = make_owner<Box<int>>();
    auto made = create<Box<int>>();
    auto closure = [] { return 7; };⟧
    §(deduced)⟦auto plain = compute();
    auto inferred = Box(1, 2);
    auto copied = auto(plain);
    auto table = make_table<long>();⟧
}
