#include "lib.h"

template <class T>
struct Point {
    T x;
};

template <class T>
void f(lib::vector<Point<T>>& pts, lib::map<int, T>& m) {
    pts.§(push_back)push_back(Point<T>{});
    auto value = m.find(1)->§(second)second;
    typename lib::vector<Point<T>>::§(value_type)value_type copy = pts.front();
}
