// Every form of write: an assignment, a compound assignment, an increment or
// a decrement, through parentheses or in a chain. A declaration is text with
// or without an initializer.

int tally(int limit) {
    int §(count)count = 0;
    int next = count;
    count = next;
    count += limit;
    ++count;
    count--;
    (count) = count * 2;
    next = count = limit;
    int §(spare)spare;
    spare = count;
    return count + spare;
}
