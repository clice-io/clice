int counter = 0;

void bump(int& target) {
    static int calls = 0;
    calls += 1;
    target += 1;
}
