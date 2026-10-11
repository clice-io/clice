void modify(int& out);
void modify_through(int* out);
void inspect(const int& in);

void run() {
    int value = 0;
    modify(§(by_reference)value);
    modify_through(&§(by_pointer)value);
    inspect(§(by_const_reference)value);
}
