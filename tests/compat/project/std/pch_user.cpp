import std;
import compat.pch;

int pch_user() {
    std::vector<int> values{pch_unit()};
    return values.front();
}
