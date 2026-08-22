/// - verify: server

int §(int_def)select(int value) {
    return value;
}

double §(double_def)select(double value) {
    return value;
}

int choose() {
    return §(int_use)select(1) + static_cast<int>(§(double_use)select(2.0));
}
