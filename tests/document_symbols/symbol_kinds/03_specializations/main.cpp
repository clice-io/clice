namespace spec {

template <typename T>
struct Box {
    T value;
};

template <>
struct Box<void> {};

template <typename T>
struct Box<T*> {
    T* pointee;
};

template <typename T>
T zero() {
    return T();
}

template <>
int zero<int>();

template <typename T>
constexpr T pi = T(3);

template <>
constexpr int pi<int> = 3;

template <typename T>
constexpr T* pi<T*> = nullptr;

template <typename T>
struct Deduced {
    Deduced(T raw);
};

template <typename T>
Deduced(T*) -> Deduced<T>;

Box<int> instantiated;

template struct Box<char>;
template long zero<long>();

}  // namespace spec
