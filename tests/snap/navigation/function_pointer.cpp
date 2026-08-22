/// - verify: server

int §(target_def)target(int value) {
    return value + 1;
}

using Callback = int (*)(int);

Callback §(pointer_def)callback = &§(address_use)target;

int invoke_pointer() {
    return §(pointer_call)callback(3);
}
