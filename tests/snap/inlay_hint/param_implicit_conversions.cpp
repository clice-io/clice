/// # Parameter Hints
///
/// ## Implicit constructor calls — conversions the code never wrote produce no hints of their own
///
/// - status: supported
/// - order: 12

struct Seconds {
    Seconds(int raw);
};

void wait(Seconds);
void hold(Seconds duration);

Seconds use() {
    // The implicit Seconds(5) must not surface `raw:`.
    wait(5);
    // The written call still hints its own parameter.
    hold(6);
    // Nor does the conversion in a return statement.
    return 7;
}
