// A return type naming a nested type or another namespace is spelled so it
// resolves at the definition; parameter types, looked up in the class scope
// there too, stay as written.

namespace geometry {
struct Point {};
}

namespace app {
struct Shape {
    struct Iterator {};
    Iterator §(begin)begin();
    const geometry::Point& §(origin)origin(Iterator it) const;
};
}

template <class T>
struct Vec {};

struct Table {
    using Row = int;
    struct Cell {
        int x;
    };
    Row (*§(function_pointer)lookup())(int);
    const Row (Table::*§(member_function_pointer)select(int x) const noexcept)();
    Row (Cell::*§(nested_member_pointer)reader())();
    Vec<Vec<int>> §(nested_arguments)rows();
    const Vec<Vec<int>>& §(nested_reference)view();
    int Cell::*§(data_member_pointer)field();
};
