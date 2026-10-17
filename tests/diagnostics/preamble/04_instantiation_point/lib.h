namespace lib {

template <typename T>
void show(T value) {
    describe(value);
}

}  // namespace lib

namespace app {

struct Point {};

}  // namespace app

inline void demo() {
    lib::show(app::Point{});
}
