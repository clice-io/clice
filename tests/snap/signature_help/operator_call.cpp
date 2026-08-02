/// # Special Call Contexts
///
/// ## Functor call — invoking an object routes signature help to its operator() overload
///
/// - status: supported
/// - order: 11

struct Adder {
    int operator()(int a, int b);
};

int main() {
    Adder add;
    add(§(pos)1, 2);
}
