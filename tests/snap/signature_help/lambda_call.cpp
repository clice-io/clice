/// # Special Call Contexts
///
/// ## Lambda call — calling a lambda variable offers the closure's operator() parameters
///
/// - status: supported
/// - order: 12

int main() {
    auto square = [](int n) {
        return n * n;
    };
    square(§(pos)3);
}
