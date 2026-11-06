namespace lib {
void helper(int value) {}
void helper(double value) {}
}  // namespace lib

using lib::helper;

template <typename T>
void call(T value) {
    §(overload_set)helper(value);
}

template <typename T>
struct Base {
    void act() {}
};

template <typename T>
struct Derived : Base<T> {
    using Base<T>::act;

    void run() {
        this->§(using_member)act();
    }
};
