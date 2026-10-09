// Between a name and punctuation the name is the starting point; between
// two names or two punctuators, the right one; in blank space, a comment
// or a qualifier, what encloses it.

int helper(int value);

§(top_const)const int limit = 3;

struct Probe {
    int get() §(trailing_const)const;
    int field;
};

int probe(int count) {
    int result = helper§(name_paren)(count);
    result = count§(name_semi);
    result = result+§(punct_name)count;
    §(local_const)const int copy = result;
    return result; §(blank)
}
