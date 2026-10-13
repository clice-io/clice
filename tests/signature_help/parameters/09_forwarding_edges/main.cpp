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
    Grid(int rows, int columns);
};

void build() {
    auto derived = std::make_unique<Derived>(§(inherited)1, 2);
    auto socket = std::make_unique<Socket>(§(public_only)80);
    std::vector<Point> points;
    points.emplace_back(1, 2, §(too_many)3);
    std::optional<Grid> grid;
    grid.emplace(§(initializer_list)2, 3);
}
