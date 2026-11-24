struct Widget {
    Widget(int size);
};

using Alias = Widget;

template <typename T>
struct Box {
    Box(T value);
};

void build() {
    auto a = §(alias)Alias(1);
    auto b = §(deduced)Box(2);
    §(declarator)Box c(3);
}
