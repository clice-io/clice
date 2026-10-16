void report(int value);

int caller() {
    int unused = 0;
    report(1, 2);
    return missing;
}
