import std;
import compat.shapes;

int use_shapes() {
    std::vector<compat::Circle> circles{{1.0}, {2.0}};
    auto grown = compat::grow(circles, 0.5);
    return static_cast<int>(compat::describe(grown.front()).size());
}
