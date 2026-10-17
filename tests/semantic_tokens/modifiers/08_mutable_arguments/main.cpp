void modify(int& out);
void modify_through(int* out);
void inspect(const int& in);
void peek(const int* in);

void run() {
    int value = 0;
    modify(§(by_reference)value);
    modify_through(&§(by_pointer)value);
    inspect(§(by_const_reference)value);
    peek(&§(by_pointer_to_const)value);

    int values[4] = {};
    modify_through(§(by_array_decay)values);
    peek(§(by_array_decay_to_const)values);
}
