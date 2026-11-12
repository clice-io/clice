// Declarations of a type that does not exist show as written, not as the
// `int` clang puts in its place; an initializer that does not compile is left
// out, a default argument that does not convert is not. A field after a
// broken one, a binding of what does not decompose, a method whose body or
// return type does not compile and a parameter whose declaration's default
// does not compile keep their cards.

Unknown §(variable)missing;

Unknown §(unknown_with_initializer)fallback = undeclared_fallback;

void consume(Missing §(parameter)value);

struct Record {
    Undeclared §(field)member;
    int §(after_broken_field)count;
};

void reconnect(int §(broken_default)delay = undeclared_delay);

struct Limits {
    int §(broken_initializer)ceiling = undeclared_ceiling;
};

auto [§(broken_binding)lone] = 1;

class Socket {};
void open(Socket §(unconverted_default)socket = nullptr);

struct Delay {};
struct Connection {
    void retry(Delay delay = undeclared_delay);
};
void Connection::retry(Delay §(definition_parameter)delay) {}

template <typename T> T convert(T);
struct Gauge {
    int level;
    void §(broken_body)set_level(float) { level = convert(undeclared_level); }
};

struct Reply {
    struct { int x; } method();
    int field;
};
§(defined_in_return_type)Reply reply;
