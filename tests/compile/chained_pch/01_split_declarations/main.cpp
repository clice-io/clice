#include "extra.h"

int run() {
    geo::Point point{1, 2};
    geo::§(move)move(point, 3);
    §(adl)move(point, 4L);
    int scaled = geo::§(scale)scale(§(s_scale)4);
    geo::Color color = geo::Color::§(red)red;
    geo::Circle circle{};
    use::§(report)report(2.0);
    geo::§(size)Size size{2, 3};
    int total = scaled + point.§(x)x + geo::§(c_geo)counter2 + static_cast<int>(color);
    total += static_cast<int>(circle.§(area)area()) + §(base_area)base_area(size);
    total += geo::Registry::§(count)count + geo::Registry::§(limit)limit;
    total += geo::§(bonus)local_bonus() + geo::§(seed)local_seed();
    geo::Shape& shape = circle;
    total += static_cast<int>(shape.§(virtual)area()) + point.§(c_point)y;
    return total;
}
