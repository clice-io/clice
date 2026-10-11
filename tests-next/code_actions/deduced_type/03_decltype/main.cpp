int value();

void f() {
    §(dt)decltype(value()) x = 0;
    §(nested)decltype(x) y = 0;
}
