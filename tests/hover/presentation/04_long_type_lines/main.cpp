namespace long_types {

template <typename... Ts> struct Tuple {};
template <typename K, typename V> struct Map {};
template <typename T> struct Vector {};
template <typename T> struct Optional {};

struct Coordinate {};
struct Measurement {};

auto §(shared)index = Map<Vector<Tuple<Coordinate, Measurement>>, Optional<Tuple<Coordinate, Measurement>>>{};

auto §(wrapped)wide = Tuple<int, long, unsigned, char, short, double, float, bool, signed char, unsigned short>{};

§(deduced)auto copy = index;

auto §(repeated)pairs = Map<Vector<Tuple<Coordinate, Measurement, Coordinate>>, Vector<Tuple<Coordinate, Measurement, Coordinate>>>{};

using Dispatch = void (*)(void (*)(int), void (*)(long), void (*)(char), void (*)(short), void (*)(float),
                          void (*)(double), void (*)(bool), void (*)(unsigned), void (*)(signed char));
Dispatch §(letters)dispatch;

}
