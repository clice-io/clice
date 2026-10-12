#include "gauge.h"

template <typename T>
T read(Gauge<T>& gauge) {
    return gauge.§(dependent_getter)getLevel();
}

template <typename T>
void write(Gauge<T>& gauge, T value) {
    gauge.§(dependent_setter)setLevel(value);
}
