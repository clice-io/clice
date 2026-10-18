#include "lib.h"

template <class T>
struct Point {
    T x;
};

template <class T>
void f(lib::vector<Point<T>>& pts, lib::map<int, T>& m) {
    m.find(1)->§(members);
    pts.§(dot);
}
