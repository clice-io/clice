int §(twice_decl)twice(int value);

int §(twice_def)twice(int value) {
    return value * 2;
}

struct Machine {
    void §(start_decl)start();
    static void §(reset_decl)reset();
};

void drive(Machine machine) {
    machine.§(start_use)start();
    Machine::§(reset_use)reset();
    int four = §(twice_use)twice(2);
}
