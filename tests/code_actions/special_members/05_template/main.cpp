struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

template <typename T>
struct §(box)Box {
    Box(T value) : value(static_cast<T&&>(value)) {}

    T value;
};

template <typename T>
struct §(unique)Unique {
    Unique(T* pointer) : pointer(pointer) {}
    Unique(Unique&& other) : pointer(other.pointer) {}

    T* pointer;
};

void use(Box<int>& numbers, Box<MoveOnly>& handles) {
    Box<int> copy = numbers;
    copy = numbers;
    Box<MoveOnly> moved = static_cast<Box<MoveOnly>&&>(handles);
    moved = static_cast<Box<MoveOnly>&&>(handles);
}
