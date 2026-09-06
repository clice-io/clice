namespace ordinary_member {
struct Point {
    int row;
};
int read() {
    Point point;
    return point.§(01_field)row;
}
}

namespace initialized_member {
struct Counter {
    int value = 12;
};
int read() {
    Counter counter;
    return counter.§(02_field_init)value;
}
}

namespace static_member {
struct Limits {
    static int maximum;
};
int read() {
    return Limits::§(03_static_field)maximum;
}
}

namespace constructor_member {
struct Window {
    int height;
    Window() : §(04_member_initializer)height(480) {}
};
}

namespace old_designator {
struct Coordinates {
    int column;
};
Coordinates point = {§(05_gnu_designator)column : 3};
}

namespace standard_designator {
struct Coordinates {
    int column;
    int row;
};
Coordinates point = {.§(06_field_designator)column = 3, .row = 4};
}

namespace member_function {
struct Queue {
    int size();
};
int read(Queue& queue) {
    return queue.§(07_method_call)size();
}
}

namespace static_function {
struct Clock {
    static int now();
};
int read() {
    return Clock::§(08_static_method_call)now();
}
}
