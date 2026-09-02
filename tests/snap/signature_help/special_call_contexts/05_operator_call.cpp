/// # Functor call — invoking an object routes signature help to its operator() overload
///
/// - status: supported

struct Adder {
    int operator()(int a, int b);
};

int main() {
    Adder add;
    add(§(pos)1, 2);
}
