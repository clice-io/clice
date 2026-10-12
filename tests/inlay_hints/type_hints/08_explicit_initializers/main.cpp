template <typename T>
struct Box {
    Box(T first, T second);
};

int compute();

void use() {
    §(spelled)⟦auto widened = static_cast<long>(compute());
    auto braced = int{42};
    auto boxed = Box<int>(1, 2);
    auto listed = Box<int>{3, 4};⟧
    §(deduced)⟦auto plain = compute();
    auto inferred = Box(1, 2);
    auto copied = auto(plain);⟧
}
