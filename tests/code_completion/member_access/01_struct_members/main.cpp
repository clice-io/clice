struct Thermostat {
    int target;
    int current;
    void raise(int step);
};

int reading(Thermostat& unit) {
    return unit.§(member)current;
}
