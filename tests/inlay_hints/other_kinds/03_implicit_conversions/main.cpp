// An implicit conversion at a call site is not shown.
void process(double val);

void use() {
    process(42);
}
