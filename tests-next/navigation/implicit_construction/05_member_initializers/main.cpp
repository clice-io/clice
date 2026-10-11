struct Base {
    Base(int x);
};

struct Logger {
    Logger(int level);
};

struct App : Base {
    Logger logger;
    App() : §(base_init)Base§(base_paren)(42), §(member_init)logger§(member_paren)(1) {}
};
