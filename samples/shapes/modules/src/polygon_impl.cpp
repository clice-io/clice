module shapes;

import :detail;

namespace shapes {

double edge(double length) {
    return detail::scale(length);
}

double Polygon<3>::measure() const {
    return 3 * edge(length);
}

const char* Polygon<3>::name() const {
    return "triangle";
}

template class Polygon<4>;
template class Polygon<6>;

}  // namespace shapes
