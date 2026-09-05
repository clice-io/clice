/// # Active parameter past a shorter overload
///
/// - status: supported
///
/// With the cursor in the second argument, only overloads that declare a second parameter remain

void draw();
void draw(int x);
void draw(int x, int y);

int main() {
    draw(1, §(pos)2);
}
