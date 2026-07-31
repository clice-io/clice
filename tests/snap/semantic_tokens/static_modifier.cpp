/// # Token Modifiers
///
/// ## Static — class-level members and static locals
///
/// - status: supported
/// - order: 2

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
