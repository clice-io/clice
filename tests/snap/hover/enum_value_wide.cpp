// A constant of an enumeration over a 64-bit unsigned type shows the
// enumerator it equals, also when the value needs all 64 bits.

enum Wide : unsigned long long { Max = ~0ULL };
enum Narrow : unsigned long long { Small = 5 };

constexpr Wide §(wide)wide = Max;
constexpr Narrow §(narrow)narrow = Small;
