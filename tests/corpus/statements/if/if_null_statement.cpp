// if with null statement and single-statement bodies
namespace if_null {

// null init-statement in C++17 if
int with_null_init(int x) {
    if(; x > 0) {
        return x;
    }
    return 0;
}

// single statement body (no braces)
int single_stmt(int x) {
    if(x > 10)
        return x;
    if(x > 5)
        return x + 1;
    return 0;
}

// scope of variable declared in if body
void scope_test() {
    if(true) {
        [[maybe_unused]] int x = 42;
    }
    // x is out of scope here

    if(int y = 10; y > 0) {
        [[maybe_unused]] int z = y;
    }
    // y and z are out of scope here
}

void test() {
    [[maybe_unused]] int r1 = with_null_init(5);
    [[maybe_unused]] int r2 = single_stmt(7);
    scope_test();
}

}  // namespace if_null
