#include <string>

int lto_probe_lib(int x) {
    std::string s(x, 'a');
    return static_cast<int>(s.size()) * 3;
}
