namespace std {
template <class Char>
struct basic_string {};

typedef basic_string<char> string;
}  // namespace std

template <class T>
struct Box {
    T values[2];
    void (*visit)(T);
};

void use(Box<std::string>& box) {
    auto& values = box.values;
    auto visit = box.visit;
}
