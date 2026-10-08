// Between a name and punctuation the name is the starting point; between
// two names or two punctuators, the right one; in blank space or a
// comment, what encloses it.

int helper(int value);

int probe(int count) {
    int result = helper§(name_paren)(count);
    result = count§(name_semi);
    result = result+§(punct_name)count;
    return result; §(blank)
}
