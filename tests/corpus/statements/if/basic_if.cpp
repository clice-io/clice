// basic if and if-else
namespace basic_if {

int abs_val(int x) {
    if(x < 0)
        return -x;
    return x;
}

const char* sign(int x) {
    if(x > 0) {
        return "positive";
    } else if(x < 0) {
        return "negative";
    } else {
        return "zero";
    }
}

// dangling else: else binds to nearest if
int nested_if(int a, int b) {
    if(a > 0)
        if(b > 0)
            return 1;
        else
            return 2;
    return 0;
}

void test() {
    [[maybe_unused]] int r1 = abs_val(-3);
    [[maybe_unused]] auto r2 = sign(5);
    [[maybe_unused]] int r3 = nested_if(1, -1);
}

}  // namespace basic_if
