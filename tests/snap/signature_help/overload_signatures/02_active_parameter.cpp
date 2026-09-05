/// # Active parameter tracking
///
/// - status: supported
///
/// The parameter under the cursor is bracketed; the point sits in the second
/// argument

void bar(int first, double second, char third);

int main() {
    bar(1, §(pos)2.0, 'c');
}
