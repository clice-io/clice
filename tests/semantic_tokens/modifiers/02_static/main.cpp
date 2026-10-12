struct Counter {
    static int §(total_decl)total;
    static void §(bump_decl)bump();
    int §(current)current;
};

void count() {
    static int §(calls_def)calls = 0;
    Counter::§(bump_use)bump();
    Counter::§(total_use)total = §(calls_use)calls;
}
