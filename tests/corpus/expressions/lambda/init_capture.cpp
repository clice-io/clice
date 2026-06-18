// lambda init-captures
namespace lambda_init_capture {

struct InterestingType {};

InterestingType foo(int);
int compute();

void issue_example() {
    auto f = [val = compute()] {
    };
    (void)f;
}

void test(int x) {
    auto lambda = [interesting = foo(x), braced{foo(x)}, &ref = x, pointer = &x]() {
        return pointer + ref;
    };
}

}  // namespace lambda_init_capture
