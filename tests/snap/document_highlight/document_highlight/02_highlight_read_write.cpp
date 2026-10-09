/// # Read and write access
///
/// - status: supported
///
/// Highlights tell writes from reads: an assignment, a compound assignment,
/// an increment or a decrement writes the name
///
/// Every other use reads it. A declaration is neither and highlights as
/// plain text, with or without an initializer.

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
