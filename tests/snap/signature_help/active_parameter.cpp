/// # Overload Signatures
///
/// ## Active parameter tracking — the parameter under the cursor is bracketed; the point sits in the second argument
///
/// - status: supported
/// - order: 2

void bar(int first, double second, char third);

int main() {
    bar(1, §(pos)2.0, 'c');
}
