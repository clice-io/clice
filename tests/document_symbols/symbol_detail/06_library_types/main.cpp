#include <map>
#include <memory>
#include <string>
#include <vector>

auto table = std::make_unique<std::map<int, std::string>>();

void lookup(const std::map<int, std::string>& entries, std::string_view key);

auto names() {
    return std::vector<std::string>{};
}
