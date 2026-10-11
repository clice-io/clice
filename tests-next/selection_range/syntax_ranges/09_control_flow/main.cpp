// A condition, a loop header's parts and a subscript grow to the statement
// they belong to.
int sum(const int* values, int count, bool ready) {
    int total = 0;
    if (§(condition)ready && count > 0) {
        for (int i = 0; i < §(bound)count; ++i) {
            total += values[§(index)i];
        }
    }
    return total;
}
