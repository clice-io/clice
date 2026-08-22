/// - verify: server

template <typename T>
T §(primary)identity(T value) {
    return value;
}

template <>
int §(spec)identity<int>(int value) {
    return value + 1;
}

template <typename T>
struct §(class_primary)Box {
    T §(member_decl)get();
    T value;
};

template <typename T>
T Box<T>::§(member_def)get() {
    return value;
}

int drive() {
    §(class_use)Box<int> box;
    box.value = 1;
    return §(use_spec)identity(2) + static_cast<int>(§(use_primary)identity(2.5)) +
           box.§(member_use)get();
}
