#include <map>
#include <memory>
#include <vector>

struct Widget {
    Widget(int width, int height);
    Widget(const char* name);
};

struct Point {
    int x;
    int y;
};

void build() {
    auto widget = std::make_unique<Widget>(§(make_unique)800, 600);
    auto shared = std::make_shared<Widget>(800, §(make_shared)600);
    std::vector<Widget> widgets;
    widgets.emplace_back(§(emplace_back)800, 600);
    std::vector<Point> points;
    points.emplace_back(1, §(aggregate)2);
    std::map<int, Widget> named;
    named.try_emplace(1, §(try_emplace)"main");
}
