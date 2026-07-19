// Seed case for the per-feature snapshot corpus: nested symbol tree.
namespace demo {

struct Point {
    int x;
    int y;

    int manhattan() const {
        return x + y;
    }
};

enum class Axis { X, Y };

int origin_distance(const Point& p);

namespace inner {
constexpr int level = 2;
}

}  // namespace demo
