#include <memory>

struct Base {
    Base(int width, int height);
};

struct Derived : Base {
    using Base::Base;
};

class Socket {
public:
    Socket(int port);
    Socket(const Socket&) = delete;

private:
    Socket(int port, int backlog);
};

struct Point {
    int x;
    int y;
};

struct Grid {
    Grid(int rows);
    Grid(int rows, int columns);
};

template <typename T>
void fill(std::vector<Grid>& grids, T rows) {
    grids.emplace_back(rows, §(dependent)1);
}

void build() {
    auto derived = std::make_unique<Derived>(§(inherited)1, 2);
    auto socket = std::make_unique<Socket>(§(public_only)80);
    auto listening = std::make_unique<Socket, int>(§(explicit_pack)80);
    std::vector<Point> points;
    points.emplace_back(1, 2, §(too_many)3);
    std::optional<Grid> grid;
    grid.emplace(§(initializer_list)2, 3);
    grid.emplace(2, §(dependent_return)3);
}
