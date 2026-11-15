export module compat.shapes;

export import :geometry;
import std;

export namespace compat {

std::string describe(const Circle& circle);

std::vector<Circle> grow(std::span<const Circle> circles, double by);

}  // namespace compat
