#include <vector>
import std;

int mixed() {
    std::vector<int> values{3, 1, 2};
    std::ranges::sort(values);
    return values.front();
}
