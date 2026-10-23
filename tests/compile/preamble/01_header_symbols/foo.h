inline void foo() {}
inline void bar() { foo(); }
#define CALLER(name) inline void name() { foo(); }
CALLER(baz)

struct Box {
    int value;
    int get() const { return value; }
};
