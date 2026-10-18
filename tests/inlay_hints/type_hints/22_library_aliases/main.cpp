namespace std {
typedef unsigned long size_t;
typedef long int64_t;

template <class Pointer, class Container>
struct __normal_iterator {};

template <class T>
struct vector {
    using reference = T&;
    using size_type = size_t;
    using iterator = __normal_iterator<T*, vector>;
    reference front();
    size_type size() const;
    iterator begin();
};

template <class Rep>
struct duration {
    using rep = Rep;
    rep count() const;
};

struct steady_clock {
    using time_point = int64_t;
    static time_point now();
};
}  // namespace std

using UserId = std::int64_t;
UserId current_user();
std::int64_t big();

void use(std::vector<int>& values, std::duration<long> elapsed) {
    auto count = values.size();
    auto& first = values.front();
    auto it = values.begin();
    auto ticks = elapsed.count();
    auto now = std::steady_clock::now();
    auto user = current_user();
    auto large = big();
}
