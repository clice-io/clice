// The modifier marks the name an argument writes: a member, the operand of
// `&`, the argument of a constructor or a call operator; never a value
// passed by copy, a function, nor one a template's parameter binds before
// knowing whether it is const.

struct Counter {
    int hits;
    Counter(int& source);
    void operator()(int& target);
};

void bump(int& out);
void copy(int in);
void notify();
void call(void (*callback)());
void call_back(void (&callback)());

void run(Counter& counter) {
    int value = 0;
    bump(counter.§(member)hits);
    Counter made(§(constructor)value);
    counter(§(call_operator)value);
    copy(§(by_value)value);
    call(§(function_pointer)notify);
    call_back(§(function_reference)notify);
}

template <typename T>
struct Holder {
    static void store(T& out);

    static void run() {
        int local = 0;
        store(§(dependent_parameter)local);
    }
};
