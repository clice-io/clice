struct Holder {
    template <typename T>
    friend class Seq;
};

template <typename T>
class Seq {};

Seq<int> make();

void use() {
    auto seq = make();
}
