struct Constructible {
    Constructible(int amount);
};

Constructible built{5};

struct Copyable {
    int x;
};

Copyable original{1};
Copyable duplicate{original};

struct Wide {
    int fields[8];
};

Wide zeroed{};
