struct Config;
struct Missing;

Config §(load)load();

struct §(panel)Panel {
    void §(take)take(Config config);
    void §(make)make(Missing missing);
    void plain();
};

struct Config {
    int x;
};
