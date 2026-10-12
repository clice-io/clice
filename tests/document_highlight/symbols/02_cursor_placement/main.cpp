// A cursor touching the end of a name and the start of punctuation belongs
// to the name; keywords, literals and blank space name nothing.

struct Sample {
    int member;
};

int probe() {
    Sample sample;
    sample§(name_end).member = 42;
    §(keyword)return sample.member + §(literal)1;§(blank)
}
