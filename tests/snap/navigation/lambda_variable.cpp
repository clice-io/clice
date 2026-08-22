/// - verify: server

int apply_lambda(int seed) {
    int §(captured_def)base = seed;
    auto §(lambda_def)add = [§(capture_use)base](int value) {
        return base + value;
    };
    return §(lambda_call)add(1);
}
