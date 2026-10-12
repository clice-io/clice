// With the cursor on the first argument of an empty call, every overload
// lists, the nullary one without a marked parameter.

void foo();
void foo(int x);
void foo(int x, int y);

int main() {
    foo(§(pos));
}
