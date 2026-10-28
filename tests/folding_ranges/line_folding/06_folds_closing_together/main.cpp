struct Item {
    int a;
    int b;
};

void each(int count, void (*visit)(int));
void push(Item item);
int add(int a, int b);

void demo() {
    each(3, [](int value) {
        value += 1;
    });
    push({
        1,
        2,
    });
    int sum = add(add(
        1,
        2), 3);
}
