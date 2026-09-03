/// # Declaration vs definition — the modifier distinguishes the two
///
/// - status: supported

int §measure(int value);

int §measure(int value) {
    return value;
}

struct §Sensor;

struct §Sensor {};
