/// - verify: server

int §(leaf)settle(int value) {
    return value;
}

int §(mid)route(int value) {
    return settle(value) + settle(value + 1);
}

int §(top)start(int value) {
    return route(value);
}

int §(recursive)restart(int value) {
    return route(value) + restart(value - 1);
}

int §(gate)cursor = 0;
