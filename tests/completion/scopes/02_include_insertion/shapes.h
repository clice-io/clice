#pragma once

namespace shapes {

struct Circle {
    double radius;
    static int circle_count;
};

void on_resize(int width);
void on_reset();

int circle_total = 0;
void redraw();
void redraw(int times);

template <class T>
struct Grid {};

template <class T>
using Row = Grid<T>;

template <class T>
constexpr int grid_size = 4;

enum Shade { shade_dark };

enum class Tone { tone_bright };

}  // namespace shapes
