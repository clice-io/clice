struct Options {
    Options();
    Options(int level, bool verbose);
};

struct Config {
    Options §(empty)defaults = {};
    Options §(arguments)tuned = {2, true};
};
