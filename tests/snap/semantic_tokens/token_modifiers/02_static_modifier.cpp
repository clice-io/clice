/// # Static
///
/// - status: supported
///
/// class-level members and static locals

struct Counter {
    static int §total;
    static void §bump();
    int §current;
};

void count() {
    static int §calls = 0;
    Counter::§bump();
    Counter::§total = §calls;
}
