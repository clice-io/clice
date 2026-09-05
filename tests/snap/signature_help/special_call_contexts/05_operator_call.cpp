/// # Functor call
///
/// - status: supported
///
/// Invoking an object routes signature help to its operator() overload

struct Adder {
    int operator()(int a, int b);
};

int main() {
    Adder add;
    add(§(pos)1, 2);
}
