namespace pointer_chain {
void inspect() {
    auto predicate = [](long value, bool ready) { return value > 0 && ready; };
    auto* pointer = &predicate;
    auto* §(01_lambda_ptr_ptr)outer = &pointer;
}
}

namespace reference_parameter {
auto operation = [](long value, bool ready) { return value > 0 && ready; };
void invoke(decltype(operation)& callable) {
    calla§(02_lambda_decltype_ref_param)ble(1, true);
}
}

namespace value_parameter {
auto operation = [](long value, bool ready) { return value > 0 && ready; };
void invoke(decltype(operation) callable) {
    calla§(03_lambda_decltype_param)ble(1, true);
}
}

namespace local_closure {
bool inspect() {
    int threshold = 6;
    auto above = [&threshold](int value) { return value > threshold; };
    return abo§(04_lambda_variable)ve(9);
}
}

namespace closure_body {
void inspect() {
    auto operation = [] {
        long loc§(05_local_in_lambda)al = 1;
    };
}
}
