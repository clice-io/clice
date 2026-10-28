// A prefix made of non-ASCII identifier characters is replaced, not inserted
// before.
int 变量一 = 0;

void bar() {
    int v = 变§(pos);
}
