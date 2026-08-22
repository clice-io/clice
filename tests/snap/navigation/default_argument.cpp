/// - verify: server

int §(seed_def)seed = 4;

int choose_default(int value = §(default_use)seed);

int §(fn_def)choose_default(int value) {
    return value;
}

int run_default() {
    return §(call_use)choose_default();
}
