import std;

int use_std() {
    std::vector<int> values{1, 2, 3};
    return static_cast<int>(std::ranges::max(values));
}
