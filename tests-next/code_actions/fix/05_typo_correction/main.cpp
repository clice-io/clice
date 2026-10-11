// The compiler's typo correction is a quick fix like any other.

int sum(int count) {
    int total = 0;
    total += §(typo)totl + count;
    return total;
}
