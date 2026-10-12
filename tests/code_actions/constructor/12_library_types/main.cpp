// Fields of standard library types: a move-only one is taken by value and
// moved from, a copyable class by const reference. The library headers
// already declare std::move, so no include is added.

#include <memory>
#include <string>

struct §(node)Node {
    std::unique_ptr<Node> next;
    std::string name;
};
