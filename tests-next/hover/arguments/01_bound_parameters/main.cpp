namespace parameter_binding {
void resize(long height, long& previous, long flags = 0);

void apply() {
    long height = 720;
    long previous = 0;
    resize(§(by_name)height, §(by_ref)previous, §(literal)2);
}
}
