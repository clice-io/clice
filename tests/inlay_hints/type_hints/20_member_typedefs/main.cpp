namespace std {
template <class Pointer, class Container>
struct normal_iterator {};

template <class T>
struct vector {
    using iterator = normal_iterator<T*, vector>;
    using size_type = unsigned long;
    iterator begin();
    size_type size() const;
};
}  // namespace std

void use(std::vector<int>& values) {
    auto it = values.begin();
    auto count = values.size();
}
