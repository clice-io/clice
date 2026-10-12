template <typename T>
void touch(T t) {
    t.member();
}

template <typename T>
struct Box {
    void put(T t) {
        touch(t);
    }
};
