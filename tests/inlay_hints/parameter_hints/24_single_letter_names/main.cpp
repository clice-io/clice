struct Stack {
    void push(int x);
};

int clamp(int v, int lo, int hi);

void use(Stack& stack) {
    stack.push(1);
    clamp(5, 0, 10);
}
