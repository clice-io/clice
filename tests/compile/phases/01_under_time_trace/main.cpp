struct Holder {
    int held;
};

template <class T>
int examine(T value) {
    return value.§(member)held;
}

int use() {
    return examine(Holder{});
}
