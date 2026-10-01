// A constant of an enumeration over a wide unsigned type shows the
// enumerator it equals, also when the value needs all 64 bits or more; past
// 64 bits the name stands alone.

enum Wide : unsigned long long { Max = ~0ULL };
enum Narrow : unsigned long long { Small = 5 };

constexpr Wide §(wide)wide = Max;
constexpr Narrow §(narrow)narrow = Small;

enum Huge : unsigned __int128 { Top = static_cast<unsigned __int128>(1) << 100 };

constexpr Huge §(huge)huge = Top;
