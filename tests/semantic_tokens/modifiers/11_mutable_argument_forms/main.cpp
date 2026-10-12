// The modifier marks the name an argument writes: a member, the operand of
// `&`, the argument of a constructor or a call operator; never a value
// passed by copy.

struct Counter {
    int hits;
    Counter(int& source);
    void operator()(int& target);
};

void bump(int& out);
void copy(int in);

void run(Counter& counter) {
    int value = 0;
    bump(counter.§(member)hits);
    Counter made(§(constructor)value);
    counter(§(call_operator)value);
    copy(§(by_value)value);
}
