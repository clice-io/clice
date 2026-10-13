/// Reads the sensor once.
///
/// Returns the reading in **millivolts**.
int read_sensor(int channel);

template <typename T>
struct Probe {
    /// The last value the probe took.
    T last_value;
};

int read_count;

void run(Probe<int> probe) {
    read_§(function);
    probe.last§(instantiated_member);
}
