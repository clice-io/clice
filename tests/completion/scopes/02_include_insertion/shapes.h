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

enum Shade { shade_dark };

}  // namespace shapes
