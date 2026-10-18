#include "lib.h"

template <class T>
struct Point {
    T x;
};

template <class T>
void f(lib::vector<Point<T>>& pts) {
    pts.assign(§(assign)1, Point<T>{});
}
