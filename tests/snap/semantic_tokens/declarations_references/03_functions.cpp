/// # Functions and methods
///
/// - status: supported
///
/// declarations, definitions and call sites

int §twice(int value);

int §twice(int value) {
    return value * 2;
}

struct Machine {
    void §start();
    static void §reset();
};

void drive(Machine machine) {
    machine.§start();
    Machine::§reset();
    int four = §twice(2);
}
