namespace std {
template <class C>
struct basic_string {};

typedef basic_string<char> string;

template <class T>
struct vector {
    T& back();
    void push_back(const T& value);
    template <class... Args>
    T& emplace_back(Args&&... args);
    static constexpr T* data(unsigned offset = 0) noexcept;
    auto at(unsigned index) const -> const T&;
};

template <class T>
const T& max(const T& a, const T& b);
}  // namespace std

void use(std::vector<std::string>& names, std::string name) {
    names.§(back)back();
    names.§(push_back)push_back(name);
    names.§(emplace_back)emplace_back(name, 1);
    names.§(data)data();
    names.§(at)at(0);
    std::§(max)max(name, name);
}
