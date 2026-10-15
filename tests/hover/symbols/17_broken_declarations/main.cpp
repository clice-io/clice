// Declarations of a type that does not exist show as written, not as the
// `int` clang puts in its place; an initializer that does not compile is left
// out.

Unknown §(variable)missing;

void consume(Missing §(parameter)value);

struct Record {
    Undeclared §(field)member;
};

void reconnect(int §(broken_default)delay = undeclared_delay);

struct Limits {
    int §(broken_initializer)ceiling = undeclared_ceiling;
};
