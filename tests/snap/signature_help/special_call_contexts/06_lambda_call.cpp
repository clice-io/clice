/// # Lambda call — calling a lambda variable offers the closure's operator() parameters
///
/// - status: supported

int main() {
    auto square = [](int n) {
        return n * n;
    };
    square(§(pos)3);
}
