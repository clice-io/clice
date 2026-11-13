void configure(int first, const char* name = "this default is longer than the limit");
void place(int at, int first = 1, int second = 2);
void log(int level, bool flush = true);

void use() {
    configure(1);
    place(0);
    log(2);
}
