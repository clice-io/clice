template <typename T>
struct Box {
    Box(T value) : value(value) {}
    T value;
};

§(plain)Box(const char*) -> Box<int>;

template <typename T>
§(templated)Box(T*) -> Box<T>;
