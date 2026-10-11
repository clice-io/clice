namespace argument_transfer {
void consume(long value);
void mutate(long& value);
void observe(const long& value);

void apply() {
    long count = 1;
    consume(§(value)count);
    mutate(§(ref)count);
    observe(§(const_ref)count);
}
}
