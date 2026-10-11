// Cards of names whose type is a lambda: through pointers, as parameters
// declared with decltype, a capturing local, a local in a lambda body.

namespace pointer_chain {
void inspect() {
    auto predicate = [](long value, bool ready) { return value > 0 && ready; };
    auto* pointer = &predicate;
    auto* §(lambda_ptr_ptr)outer = &pointer;
}
}

namespace reference_parameter {
auto operation = [](long value, bool ready) { return value > 0 && ready; };
void invoke(decltype(operation)& callable) {
    calla§(lambda_decltype_ref_param)ble(1, true);
}
}

namespace value_parameter {
auto operation = [](long value, bool ready) { return value > 0 && ready; };
void invoke(decltype(operation) callable) {
    calla§(lambda_decltype_param)ble(1, true);
}
}

namespace local_closure {
bool inspect() {
    int threshold = 6;
    auto above = [&threshold](int value) { return value > threshold; };
    return abo§(lambda_variable)ve(9);
}
}

namespace closure_body {
void inspect() {
    auto operation = [] {
        long loc§(local_in_lambda)al = 1;
    };
}
}
