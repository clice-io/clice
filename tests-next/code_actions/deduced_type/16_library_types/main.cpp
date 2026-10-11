// Library types as everyday code meets them: an alias keeps its name, a
// default template argument is left out, a member typedef is reached
// through its class, a range-for variable takes the element type.
// `decltype(auto)` and an abbreviated template parameter offer nothing.

#include <utility>
#include <vector>

using Id = int;
Id next_id();
std::vector<int> numbers();
std::pair<int, double> both();

void f() {
    §(alias)auto id = next_id();
    §(default_argument)auto list = numbers();
    §(two_arguments)auto p = both();
    std::vector<int> v;
    §(member_typedef)auto it = v.begin();
    for (§(range_for)auto& x : v) {
    }
    §(decltype_auto)decltype(auto) d = next_id();
}

void g(§(abbreviated)auto value) {}

struct Limits {
    static constexpr §(static_member)auto most = 10;
};
