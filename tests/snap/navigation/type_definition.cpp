/// - verify: server

struct §(type)Point {
    int x;
    int y;
};

using §(alias)Alias = Point;

Point §(var)origin;
Alias §(alias_var)corner;

int probe() {
    §(auto_var)auto copy = origin;
    return copy.x;
}
