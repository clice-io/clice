namespace lambda_auto_params {

struct Point {
    int x;
    int y;
};

auto norm = [](§(placeholder)auto §(parameter)point) {
    return §(parameter_use)point.§(member_use)x + point.y;
};

int length = norm(Point{3, 4});

}
