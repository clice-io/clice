struct Config {
    static int shared_count;
    static int make(int seed);

    struct Nested {
        int a;
    };

    int instance_field;
};

void bar() {
    int v = Config::§(pos);
}
