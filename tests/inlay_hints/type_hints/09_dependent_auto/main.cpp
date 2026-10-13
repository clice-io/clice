template <typename T>
struct Wrapper {};

template <typename T, int N>
struct Grid {
    static auto make() -> Grid;
};

template <typename T>
void body(T input) {
    auto copy = input;
    auto grid = Grid<T, 2>::make();
    auto wrapped = Wrapper<T>{};
    auto derived = input + 1;
    auto counter = 0;
}
