// - diagnostics: expected

struct Destination {
    int count = 0;
};

struct WrongType {};

constexpr Destination invalid = §(01_invalid_init_list){
    .count = WrongType(),
};
