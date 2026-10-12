// Argument cards past the plain cases: through a forwarding template, through a
// converting constructor, at a literal and an expression, at a call that leaves
// a default argument out.

namespace forwarded_argument {
struct Record {
    explicit Record(long identifier) {}
};
template <typename T, typename... Args> T construct(Args&&... args) {
    return T(args...);
}
void run() {
    long identifier = 4;
    auto record = construct<Record>(§(forwarded_arg)identifier);
}
}

namespace converted_argument {
void measure(const double& distance);
void run() {
    int distance = 5;
    measure(§(converted_arg)distance);
}
}

namespace converted_constructor_argument {
struct Measurement {
    explicit Measurement(const double& distance) {}
};
void run() {
    int distance = 6;
    Measurement measurement(§(converted_ctor_arg)distance);
}
}

namespace literal_argument {
void compare(long actual, const long& expected) {}
void run() {
    compare(7, §(literal_arg)8);
}
}

namespace expression_argument {
void compare(long actual, const long& expected) {}
void run() {
    compare(7, 8 §(expression_arg)+ 9);
}
}

namespace value_expression {
long add(long left, long right);
long run() {
    return add(2 §(expression_by_value)+ 3, 4);
}
}

namespace converted_literal {
void measure(const double& distance);
void run() {
    measure(§(converted_literal)10);
}
}

namespace method_argument {
class Logger {
public:
    void write(long level = 1, long flags = 0) {}
};
void run() {
    long level = 2;
    Logger logger;
    logger.write(§(method_arg_default)level);
}
}

namespace converting_constructor {
struct Key {
    Key(const int& value);
};
void lookup(Key key);
void run() {
    const int value = 11;
    lookup(§(converting_ctor_arg)value);
}
}
