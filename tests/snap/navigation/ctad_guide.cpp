/// - verify: server

template <typename T>
struct §(class_def)Box {
    Box(T input) : value(input) {}
    T value;
};

template <typename T>
§(guide_decl)Box(T) -> Box<T>;

int read_box() {
    §(ctad_use)Box box§(ctad_invoke)(7);
    return box.value;
}
