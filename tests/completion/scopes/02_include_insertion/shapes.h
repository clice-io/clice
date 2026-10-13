#pragma once

namespace shapes {

struct Circle {
    double radius;
    static int circle_count;
};

void on_resize(int width);
void redraw();
void redraw(int times);

template <class T>
struct Grid {};

template <class T>
using Row = Grid<T>;

template <class T>
constexpr int grid_size = 4;

enum Shade { shade_dark };

}  // namespace shapes
