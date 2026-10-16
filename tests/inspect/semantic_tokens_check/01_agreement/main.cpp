#define SQUARE(x) ((x) * (x))
#define DECLARE(name) int name = 0;

namespace shapes {

template <typename T, int N>
struct Grid {
    T cells[N];

    ~Grid() = default;
    T& operator[](int index);
    Grid& operator=(const Grid& other);
};

using Small = Grid<int, 4>;

DECLARE(declared_by_macro)

}  // namespace shapes

void bump(int& target);
void bump(const double& source);

template <class T>
void touch(T value) {
    bump(value);
}

int use(shapes::Small grid, const char* label) {
    touch(1);
    shapes::Small other = grid;
    other = grid;
    other[0] = SQUARE(grid[1]);
    return label[0] == 'x' and other[0] > 0;
}
