#include <vector>

int add(int lhs, int rhs) {
    return lhs + rhs;
}

int main() {
    std::vector<int> values;
    values.emplace_back(0);
    auto n = add(1, 2);
    return n;
}
