module compat.shapes;

import std;

namespace compat {

std::string describe(const Circle& circle) {
    return std::format("circle of area {:.2f}", circle.area());
}

std::vector<Circle> grow(std::span<const Circle> circles, double by) {
    std::vector<Circle> grown;
    for(const auto& circle: circles) {
        grown.push_back({circle.radius + by});
    }
    return grown;
}

}  // namespace compat
