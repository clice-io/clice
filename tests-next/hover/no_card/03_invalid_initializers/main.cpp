struct Destination {
    int count = 0;
};

struct WrongType {};

constexpr Destination invalid = §(invalid_init_list){
    .count = WrongType(),
};
