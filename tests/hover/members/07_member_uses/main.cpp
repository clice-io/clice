// Member cards at their uses: through an object, static, in a constructor's
// initializer list, in designated initializers old and new, member calls.

namespace ordinary_member {
struct Point {
    int row;
};
int read() {
    Point point;
    return point.§(field)row;
}
}

namespace initialized_member {
struct Counter {
    int value = 12;
};
int read() {
    Counter counter;
    return counter.§(field_init)value;
}
}

namespace static_member {
struct Limits {
    static int maximum;
};
int read() {
    return Limits::§(static_field)maximum;
}
}

namespace constructor_member {
struct Window {
    int height;
    Window() : §(member_initializer)height(480) {}
};
}

namespace old_designator {
struct Coordinates {
    int column;
};
Coordinates point = {§(gnu_designator)column : 3};
}

namespace standard_designator {
struct Coordinates {
    int column;
    int row;
};
Coordinates point = {.§(field_designator)column = 3, .row = 4};
}

namespace member_function {
struct Queue {
    int size();
};
int read(Queue& queue) {
    return queue.§(method_call)size();
}
}

namespace static_function {
struct Clock {
    static int now();
};
int read() {
    return Clock::§(static_method_call)now();
}
}
