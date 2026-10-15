// Declarations of a type that does not exist show as written, not as the
// `int` clang puts in its place.

Unknown §(variable)missing;

void consume(Missing §(parameter)value);

struct Record {
    Undeclared §(field)member;
};
