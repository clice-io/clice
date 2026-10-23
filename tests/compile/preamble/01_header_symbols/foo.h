inline void foo() {}
inline void bar() { foo(); }

struct Box {
    int value;
    int get() const { return value; }
};
