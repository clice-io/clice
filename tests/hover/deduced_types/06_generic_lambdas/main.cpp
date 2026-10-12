namespace lambda_auto_params {

struct Point {
    int x;
    int y;
};

auto norm = [](§(placeholder)auto §(parameter)point) {
    return §(parameter_use)point.§(member_use)x + point.y;
};

int length = norm(Point{3, 4});

auto echo = [](auto §(several)value) {
    return value;
};

long first = echo(1L);
int second = echo(2);

}
