#include <map>
#include <memory>
#include <vector>

struct Widget {
    /// A widget of the given size, in pixels.
    Widget(int width, int height);
    Widget(const char* name, double scale);
};

struct Arena {};

struct Point {
    int x;
    int y;
};

void build() {
    auto widget = std::make_unique<Widget>(§(make_unique)800, 600);
    auto shared = std::make_shared<Widget>(800, §(make_shared)600);
    auto named = std::make_shared<Widget>("main", §(by_type)2.0);
    auto pooled = std::allocate_shared<Widget>(Arena{}, §(allocate_shared)800, 600);
    std::vector<Widget> widgets;
    widgets.emplace_back(§(emplace_back)800, 600);
    std::vector<Point> points;
    points.emplace_back(1, §(aggregate)2);
    std::map<int, Widget> by_id;
    by_id.try_emplace(1, §(try_emplace)"main", 1.0);
}
