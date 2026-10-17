namespace app {

struct Point {};

void describe(const Point&);

}  // namespace app

template <typename T>
void show(T value) {
    describe(value);
}

inline void demo() {
    show(app::Point{});
}
