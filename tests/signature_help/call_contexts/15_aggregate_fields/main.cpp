struct Flags {
    unsigned mode : 3 = 2;
    int level{1};
    const char* name = "plain";
    mutable int hits = 0;
};

int main() {
    Flags flags{1, §(fields)2};
}
