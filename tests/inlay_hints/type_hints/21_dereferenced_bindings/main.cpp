#include <map>
#include <string>

struct Point {
    int x;
    int y;
};

void look(std::map<std::string, Point>& points, std::pair<const std::string, Point>& entry) {
    §(begin)⟦auto [name, point] = *points.begin();⟧
    §(reference)⟦auto& [key, value] = *points.begin();⟧
    §(find)⟦auto [found, at] = *points.find(std::string());⟧
    §(pair)⟦auto [label, origin] = entry;⟧
}
