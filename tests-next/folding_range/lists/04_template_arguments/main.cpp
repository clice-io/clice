// A template argument list spanning lines, like a call's arguments.
template <typename Key, typename Value, typename Hash>
struct Table {};

struct Hasher {};

using Index = Table<
    int,
    long,
    Hasher>;

Table<
    char,
    short,
    Hasher> lookup;
