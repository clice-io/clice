#include <map>
#include <string>
#include <tuple>

struct Point {
    int x;
    int y;
};

using Count = unsigned long;
using Size = unsigned long;

void walk(std::map<std::string, Point>& points,
          std::tuple<std::string, Point>& entry,
          std::map<Count, Size>& sizes) {
    §(map)⟦for (auto& [name, point] : points) {⟧
    }
    §(tuple)⟦auto& [label, origin] = entry;⟧
    §(two_aliases)⟦for (auto& [count, size] : sizes) {⟧
    }
    std::string text;
    §(variable)⟦auto copy = text;⟧
}
