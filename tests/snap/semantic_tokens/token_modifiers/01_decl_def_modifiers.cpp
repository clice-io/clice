/// # Declaration vs definition
///
/// - status: supported
///
/// Declaration and definition modifiers distinguish the two sites

int §measure(int value);

int §measure(int value) {
    return value;
}

struct §Sensor;

struct §Sensor {};
