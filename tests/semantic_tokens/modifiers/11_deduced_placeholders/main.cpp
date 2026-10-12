// A placeholder colors as the type it stands for only when that type is
// named by a keyword or a class: a pointer, a type a template has not
// deduced yet and an init-capture's unwritten `auto` keep the keyword.

struct Point {
    int x;
    int y;
};

int value = 0;
§(pointer)auto pointer = &value;
§(binding)auto [first, second] = Point{};
§(function)auto make() { return Point{}; }
§(decltype_auto)decltype(auto) copy = value;

template <typename T>
void generic(T input) {
    §(dependent)auto local = input;
    §(non_dependent)auto count = 1;
}

auto lambda = [§(capture)captured = value] { return captured; };
