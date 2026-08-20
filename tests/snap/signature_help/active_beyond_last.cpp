/// # Overload Signatures
///
/// ## Active parameter past a shorter overload — with the cursor in the second argument, only overloads that declare a second parameter remain
///
/// - status: supported
/// - order: 14

void draw();
void draw(int x);
void draw(int x, int y);

int main() {
    draw(1, §(pos)2);
}
