#include <map>
#include <string>
#include <vector>

std::string report(const std::vector<std::string>& names) {
    std::map<std::string, int> counts;
    for(const auto& name: names) {
        counts[name] += 1;
    }
    std::string text;
    for(const auto& [name, count]: counts) {
        text += name + ": " + std::to_string(count) + "\n";
    }
    return text;
}
