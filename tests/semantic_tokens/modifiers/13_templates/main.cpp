template <typename T>
struct Box {
    T value;
};

template <typename T>
using Boxes = Box<T>[4];

template <typename T>
T zero = T();

template <typename T>
T unwrap(Box<T> box) {
    return box.value;
}

int use() {
    Box<int> box{1};
    return unwrap(box) + zero<int>;
}
