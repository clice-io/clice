// A local of the new name in a caller hides the renamed function there.
int compute(int x) { return x; }

int report(int count) {
    int evaluate = count;
    return compute(evaluate) + evaluate;
}
