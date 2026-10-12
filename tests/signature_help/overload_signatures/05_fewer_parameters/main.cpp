// With the cursor on the second argument, overloads that take fewer
// parameters drop out.

void draw();
void draw(int x);
void draw(int x, int y);

int main() {
    draw(1, §(pos)2);
}
